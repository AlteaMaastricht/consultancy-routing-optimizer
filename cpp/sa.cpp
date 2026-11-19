// Simulated Annealing for the student-routing problem (profit - time, 20 students, 8h shifts).
//
// Compile: g++ -O3 -march=native -std=c++17 -pthread -o sa sa.cpp
// Run:     ./sa [seconds=80] [warm_start_file] [seed]
//            ./sa 600                      -> 10 minute run from scratch
//            ./sa 600 "output RECORD.txt"  -> keep improving an existing solution (reheat cycles only)
// Reads orders.txt + drivingtimes.txt from the current directory (or from $SA_DATA_DIR), writes
// output.txt to the current directory.
// Optional environment knobs: SA_T0, SA_T1, SA_TR (temperatures in euro), SA_FRAC1 (share of the
// time spent in the main anneal), SA_CYCLE (seconds per reheat), SA_THREADS, SA_TOUR,
// SA_W="w0 .. w9" (operator weights), SA_TRACE=1 (print temperature / score trace).
//
// What changed compared to the first version (and why it is stronger):
//   * Integer objective in seconds (60*profit - time): no floating point drift, exact deltas.
//   * Order-to-order distance matrix (uint16) with the depot as a sentinel at both route ends:
//     no node indirection, no special cases for first/last position.
//   * Every insertion is a best-position insertion (O(route length) scan) instead of a random
//     position, and target routes are picked through nearest-neighbour lists.
//   * Compound moves that keep routes feasible while trading orders, which is what matters when
//     every route is full: k-out/1-in, k-out/2-in, ejection chains (new order pushes another
//     order into a different route, optionally pushing a third one out), inter-route swap with
//     re-optimised positions, or-opt with reversal, neighbour-guided 2-opt.
//   * SISR-style ruin & recreate: strings are cut out of a few neighbouring routes and all removed
//     plus nearby unassigned orders are greedily re-inserted (with rollback on rejection).
//   * Orders whose profit does not even pay for their own service time are never tried, and
//     unassigned orders are sampled with a bias towards high profit per minute.
//   * ~6x more iterations per second per thread (xorshift RNG, clock read every 16k iterations,
//     flat arrays instead of hash maps, O(1) tracking instead of rebuilds), on all cores.
//   * Time-based schedule: one long anneal (T 5 -> 0.04 euro; the old 8 -> 0.004 over a fixed
//     15M iterations cooled far too fast), then reheat cycles from the shared best solution.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------- constants
static constexpr int S        = 20;        // students
static constexpr int HQ_NODE  = 251;       // depot node id
static constexpr int MAXT     = 480 * 60;  // shift length in seconds
static constexpr int MAXN     = 1280;      // upper bound on #orders (+1 for depot)
static constexpr int MAXNODES = 1200;
static constexpr int MAXL     = 120;       // upper bound on orders per route
static constexpr int KNN      = 24;        // neighbour list size (small moves)
static constexpr int KB       = 48;        // neighbour list size (ruin & recreate)
static constexpr int BIG      = 1 << 20;

// ---------------------------------------------------------------- problem data (read-only after load)
static int N = 0;                 // number of orders
static int DEP = 0;               // index of the depot in D (== N)
static int oid[MAXN], onode[MAXN], svc[MAXN], prof[MAXN];   // svc in s, prof in 1/60 eur
static uint32_t emask[MAXN];
static std::vector<int> elig[MAXN];
static uint16_t D[MAXN][MAXN];    // order-to-order driving time (seconds)
static int NN[MAXN][KNN];
static int NNB[MAXN][KB];
static bool dead[MAXN];           // profit <= service cost: can never improve a solution
static float dens[MAXN];          // profit per second of service
static int P_TOUR = 50;           // % of unassigned picks that use a 2-tournament on density
static std::unordered_map<int, int> oid_to_idx;
static int dtn[MAXNODES][MAXNODES];
static int num_nodes = 0;
static int num_nodes_cols = 0;

// ---------------------------------------------------------------- SA parameters (euro)
static double P_T0      = 5.0;    // start temperature of the main anneal
static double P_T1      = 0.04;   // end temperature
static double P_TR      = 0.6;   // reheat temperature
static double P_FRAC1   = 0.75;   // fraction of the time budget for the main anneal
static double P_CYCLE   = 20.0;   // seconds per reheat cycle
static int    P_THREADS = 0;      // 0 = hardware concurrency

// ---------------------------------------------------------------- RNG
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 88172645463325252ULL) : s(seed ? seed : 1) { for (int i = 0; i < 8; i++) next(); }
    inline uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    inline int below(int n) { return (int)(((next() >> 32) * (uint64_t)n) >> 32); }
    inline double uni() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

// ---------------------------------------------------------------- I/O
// [Phase 2 fix] strict input parsing: every field must be a complete integer within range,
// array capacities are checked before writing, and every problem is reported as
// "<file>:<line>: <message>" followed by exit(1) (previously: std::stoi aborts / silent truncation /
// out-of-bounds writes).
[[noreturn]] static void input_error(const char* fn, int lineno, const std::string& msg) {
    std::cerr << fn;
    if (lineno > 0) std::cerr << ":" << lineno;
    std::cerr << ": " << msg << "\n";
    exit(1);
}
static bool parse_int(const std::string& tok, long long lo, long long hi, long long& out) {
    size_t b = tok.find_first_not_of(" \t\r");
    if (b == std::string::npos) return false;
    size_t e = tok.find_last_not_of(" \t\r");
    std::string t = tok.substr(b, e - b + 1);
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(t.c_str(), &end, 10);
    if (errno != 0 || end == t.c_str() || *end != '\0' || v < lo || v > hi) return false;
    out = v;
    return true;
}
static int field_int(const char* fn, int lineno, const std::string& tok, const char* what, long long lo, long long hi) {
    long long v;
    if (!parse_int(tok, lo, hi, v))
        input_error(fn, lineno, std::string("invalid integer for ") + what + ": '" + tok + "' (allowed range "
                                 + std::to_string(lo) + ".." + std::to_string(hi) + ")");
    return (int)v;
}

static void read_orders(const char* fn) {
    std::ifstream f(fn);
    if (!f) { std::cerr << "cannot open " << fn << "\n"; exit(1); }
    std::string line;
    std::getline(f, line);
    int lineno = 1;
    while (std::getline(f, line)) {
        lineno++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        std::vector<std::string> tok;
        std::istringstream ss(line);
        std::string t;
        while (std::getline(ss, t, '\t')) tok.push_back(t);
        if ((int)tok.size() < 7 + S)
            input_error(fn, lineno, "expected at least " + std::to_string(7 + S) + " tab-separated columns, got "
                                    + std::to_string(tok.size()));
        if (N >= MAXN - 1)
            input_error(fn, lineno, "too many orders (this build supports at most " + std::to_string(MAXN - 1) + ")");
        int i = N++;
        oid[i]   = field_int(fn, lineno, tok[0], "order id", INT_MIN, INT_MAX);
        onode[i] = field_int(fn, lineno, tok[2], "node id", 0, MAXNODES - 1);
        svc[i]   = field_int(fn, lineno, tok[5], "duration (min)", 0, 1000000) * 60;
        prof[i]  = field_int(fn, lineno, tok[6], "profit (eur)", -1000000, 1000000) * 60;
        emask[i] = 0;
        for (int s = 0; s < S; s++)
            if (field_int(fn, lineno, tok[7 + s], "eligibility flag", 0, 1) == 1) { emask[i] |= 1u << s; elig[i].push_back(s); }
        if (!oid_to_idx.emplace(oid[i], i).second)
            input_error(fn, lineno, "duplicate order id " + std::to_string(oid[i]));
    }
    if (N == 0) input_error(fn, 0, "no orders found");
    DEP = N;
}

static void read_times(const char* fn) {
    std::ifstream f(fn);
    if (!f) { std::cerr << "cannot open " << fn << "\n"; exit(1); }
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        if (num_nodes >= MAXNODES)
            input_error(fn, lineno, "matrix has more than " + std::to_string(MAXNODES) + " rows (build limit)");
        std::istringstream ss(line);
        std::string t;
        int c = 0;
        while (ss >> t) {
            if (c >= MAXNODES)
                input_error(fn, lineno, "row has more than " + std::to_string(MAXNODES) + " entries (build limit)");
            // D stores driving times as uint16_t, so larger values are rejected instead of wrapping
            dtn[num_nodes][c++] = field_int(fn, lineno, t, "driving time (s)", 0, 65535);
        }
        if (num_nodes > 0 && c != num_nodes_cols)
            input_error(fn, lineno, "row has " + std::to_string(c) + " entries, expected " + std::to_string(num_nodes_cols));
        num_nodes_cols = c;
        num_nodes++;
    }
    if (num_nodes == 0) input_error(fn, 0, "empty driving-time matrix");
    if (num_nodes_cols != num_nodes)
        input_error(fn, 0, "matrix is " + std::to_string(num_nodes) + "x" + std::to_string(num_nodes_cols) + ", expected a square matrix");
}

// [Phase 2 fix] the depot is fixed by the problem definition (node 251); a matrix without it, or an
// order whose node is outside the matrix, is an input error (previously: zero-initialised rows were
// read silently and the reported objective was wrong, or memory outside the matrix was read).
static void check_instance(const char* orders_fn, const char* times_fn) {
    if (HQ_NODE >= num_nodes)
        input_error(times_fn, 0, "depot node " + std::to_string(HQ_NODE) + " is not in the " + std::to_string(num_nodes)
                                 + "x" + std::to_string(num_nodes) + " driving-time matrix");
    for (int i = 0; i < N; i++)
        if (onode[i] >= num_nodes)
            input_error(orders_fn, 0, "order " + std::to_string(oid[i]) + " uses node " + std::to_string(onode[i])
                                      + ", which is not in the " + std::to_string(num_nodes) + "-node driving-time matrix");
}

static void precompute() {
    bool sym = true;
    for (int i = 0; i <= N; i++) {
        int ni = (i == N) ? HQ_NODE : onode[i];
        for (int j = 0; j <= N; j++) {
            int nj = (j == N) ? HQ_NODE : onode[j];
            D[i][j] = (uint16_t)dtn[ni][nj];
            if (dtn[ni][nj] != dtn[nj][ni]) sym = false;
        }
    }
    if (!sym) std::cerr << "WARNING: driving times are not symmetric; 2-opt / reversal deltas assume symmetry!\n";
    Rng rg(12345);
    std::vector<std::pair<int, int>> v;
    for (int i = 0; i < N; i++) {
        v.clear();
        for (int j = 0; j < N; j++)
            if (j != i) v.push_back({(int)D[i][j] * 4096 + rg.below(4096), j});   // random tie-break
        int k = std::min(KNN, (int)v.size());
        std::partial_sort(v.begin(), v.begin() + k, v.end());
        // [Phase 2 fix] with a single order there are no neighbours (k == 0 made q % k divide by zero);
        // the order itself is used as a sentinel neighbour, which every move already handles
        // (it is never in another route and is excluded where a different order is required).
        for (int q = 0; q < KNN; q++) NN[i][q] = k > 0 ? v[q % k].second : i;
        int kb = std::min(KB, (int)v.size());
        std::partial_sort(v.begin(), v.begin() + kb, v.end());
        for (int q = 0; q < KB; q++) NNB[i][q] = kb > 0 ? v[q % kb].second : i;
        dead[i] = prof[i] <= svc[i];
        // [Phase 2 fix] zero-duration orders: avoid 0/0 (only reachable with synthetic input)
        dens[i] = svc[i] > 0 ? (float)prof[i] / (float)svc[i] : (prof[i] > 0 ? 1e30f : 0.0f);
    }
}

// ---------------------------------------------------------------- shared best (between threads)
struct Shared {
    std::mutex mu;
    long long sc = LLONG_MIN;
    int len[S];
    int r[S][MAXL];
} shared;

// ---------------------------------------------------------------- solver (one per thread)
enum Op { ADD = 0, REMOVE, REPLACE, REPLACE2, RELOCATE, SWAP, TWOOPT, OROPT, EJECT, RUIN, NOP };
static const char* op_names[NOP] = {"add", "remove", "replace(k-1)", "replace(k-2)", "relocate", "swap", "2-opt", "or-opt", "eject-chain", "ruin-recreate"};
static double op_w[NOP] = {8, 2, 20, 6, 18, 16, 8, 8, 14, 0.5};

struct Solver {
    // current solution; routes carry the depot as sentinel at index 0 and len+1
    int r[S][MAXL + 4];
    int len[S], tm[S], pf[S];
    int stu[MAXN], pos[MAXN];
    int un[MAXN], upos[MAXN], nun;
    long long sc;                   // sum(pf - tm), all routes feasible at all times
    double T, invT;
    Rng rng;
    // best
    int br[S][MAXL], blen[S];
    long long bsc;
    long long opA[NOP], opS[NOP];
    long long iters = 0; bool trace = false;
    int buf1[MAXL + 8], buf2[MAXL + 8];
    uint32_t op_thr[NOP];

    // ---------- basic helpers
    inline bool el(int o, int s) const { return (emask[o] >> s) & 1u; }
    inline bool acc(int d) {
        if (d >= 0) return true;
        double x = d * invT;
        if (x < -30.0) return false;
        return rng.uni() < std::exp(x);
    }
    inline void unAdd(int o) { stu[o] = -1; if (dead[o]) return; upos[o] = nun; un[nun++] = o; }
    inline int pickUn() {
        int u = un[rng.below(nun)];
        if (rng.below(100) < P_TOUR) { int w = un[rng.below(nun)]; if (dens[w] > dens[u]) u = w; }
        return u;
    }
    inline void unRemove(int o) { int p = upos[o]; int last = un[--nun]; un[p] = last; upos[last] = p; }
    inline void setT(double t_eur) { T = t_eur * 60.0; invT = 1.0 / T; }

    // write buf[1..n] as route s
    inline void commit(int s, const int* buf, int n, int nt, int np) {
        int* a = r[s];
        a[0] = DEP;
        for (int i = 1; i <= n; i++) { int o = buf[i]; a[i] = o; pos[o] = i; stu[o] = s; }
        a[n + 1] = DEP;
        len[s] = n; tm[s] = nt; pf[s] = np;
    }
    inline void fin(int delta) {
        sc += delta;
        if (sc > bsc) saveBest();
    }
    void saveBest() {
        bsc = sc;
        for (int s = 0; s < S; s++) { blen[s] = len[s]; memcpy(br[s], r[s] + 1, sizeof(int) * len[s]); }
    }
    void loadRoutes(const int rr[S][MAXL], const int* ll) {
        nun = 0; sc = 0;
        for (int i = 0; i < N; i++) stu[i] = -1;
        for (int s = 0; s < S; s++) {
            int t = 0, p = 0, prev = DEP;
            r[s][0] = DEP;
            for (int i = 0; i < ll[s]; i++) {
                int o = rr[s][i];
                r[s][i + 1] = o; pos[o] = i + 1; stu[o] = s;
                t += D[prev][o] + svc[o]; p += prof[o]; prev = o;
            }
            t += D[prev][DEP];
            r[s][ll[s] + 1] = DEP;
            len[s] = ll[s]; tm[s] = t; pf[s] = p;
            sc += p - t;
        }
        for (int i = 0; i < N; i++) if (stu[i] < 0) unAdd(i);
    }
    void loadBest() { loadRoutes(br, blen); }

    // Best insertion of the segment with end points f..l into route array a (n orders, sentinels at
    // 0 and n+1), pretending that indices [x1..x2] were removed. Returns the added driving time;
    // bp = index after which to insert (in the original indexing), brev = insert reversed.
    inline int scan(const int* a, int n, int f, int l, int x1, int x2, int& bp, int& brev) const {
        int best = INT_MAX; bp = -1; brev = 0;
        const uint16_t* Df = D[f];
        if (f == l) {
            auto ev = [&](int p, int A, int B) {
                int c = (int)Df[A] + (int)Df[B] - (int)D[A][B];
                if (c < best) { best = c; bp = p; }
            };
            if (x1 > n) { for (int p = 0; p <= n; p++) ev(p, a[p], a[p + 1]); }
            else {
                for (int p = 0; p + 1 < x1; p++) ev(p, a[p], a[p + 1]);
                ev(x1 - 1, a[x1 - 1], a[x2 + 1]);
                for (int p = x2 + 1; p <= n; p++) ev(p, a[p], a[p + 1]);
            }
        } else {
            const uint16_t* Dl = D[l];
            auto ev = [&](int p, int A, int B) {
                int base = (int)D[A][B];
                int c1 = (int)Df[A] + (int)Dl[B] - base;
                int c2 = (int)Dl[A] + (int)Df[B] - base;
                if (c1 < best) { best = c1; bp = p; brev = 0; }
                if (c2 < best) { best = c2; bp = p; brev = 1; }
            };
            if (x1 > n) { for (int p = 0; p <= n; p++) ev(p, a[p], a[p + 1]); }
            else {
                for (int p = 0; p + 1 < x1; p++) ev(p, a[p], a[p + 1]);
                ev(x1 - 1, a[x1 - 1], a[x2 + 1]);
                for (int p = x2 + 1; p <= n; p++) ev(p, a[p], a[p + 1]);
            }
        }
        return best;
    }
    // out[1..n] = a[1..L] without [x1..x2], with ins[0..k) inserted after original index p
    inline int build(const int* a, int L, int x1, int x2, int p, const int* ins, int k, int* out) const {
        int n = 0;
        out[0] = DEP;
        if (p == 0) for (int i = 0; i < k; i++) out[++n] = ins[i];
        for (int i = 1; i <= L; i++) {
            if (i < x1 || i > x2) out[++n] = a[i];
            if (i == p) for (int j = 0; j < k; j++) out[++n] = ins[j];
        }
        out[n + 1] = DEP;
        return n;
    }
    // choose a route for order o: usually the route of one of its nearest neighbours
    inline int pickRoute(int o, int excl, int& v) {
        v = -1;
        if (rng.below(100) < 80) {
            for (int t = 0; t < 3; t++) {
                int w = NN[o][rng.below(KNN)];
                int s = stu[w];
                if (s >= 0 && s != excl && el(o, s)) { v = w; return s; }
            }
        }
        const std::vector<int>& e = elig[o];
        if (e.empty()) return -1;
        int s = e[rng.below((int)e.size())];
        return s == excl ? -1 : s;
    }
    // driving+service time change of removing a[x1..x2]; rp = removed profit
    inline int removal(const int* a, int x1, int x2, int& rp) const {
        int d = (int)D[a[x1 - 1]][a[x2 + 1]] - (int)D[a[x1 - 1]][a[x1]] - (int)D[a[x2]][a[x2 + 1]];
        rp = 0;
        for (int i = x1; i <= x2; i++) {
            d -= svc[a[i]]; rp += prof[a[i]];
            if (i < x2) d -= D[a[i]][a[i + 1]];
        }
        return d;
    }

    // ---------- moves (all keep every route within MAXT)
    bool mvAdd() {
        if (!nun) return false;
        int u = pickUn(), v;
        int s = pickRoute(u, -1, v);
        if (s < 0 || len[s] >= MAXL - 3) return false;
        int p, rev;
        int nt = tm[s] + scan(r[s], len[s], u, u, BIG, BIG, p, rev) + svc[u];
        if (nt > MAXT) return false;
        int delta = prof[u] - (nt - tm[s]);
        if (!acc(delta)) return false;
        int n = build(r[s], len[s], BIG, BIG, p, &u, 1, buf1);
        unRemove(u);
        commit(s, buf1, n, nt, pf[s] + prof[u]);
        fin(delta);
        return true;
    }
    bool mvRemove() {
        int a = rng.below(N), s = stu[a];
        if (s < 0) return false;
        int x = pos[a], rp;
        int d = removal(r[s], x, x, rp);
        if (d > 0) return false;
        int delta = -rp - d;
        if (!acc(delta)) return false;
        int n = build(r[s], len[s], x, x, -1, nullptr, 0, buf1);
        commit(s, buf1, n, tm[s] + d, pf[s] - rp);
        unAdd(a);
        fin(delta);
        return true;
    }
    // remove 1-3 consecutive orders from a route, insert one unassigned order at its best position
    bool mvReplace() {
        if (!nun) return false;
        int u = pickUn(), v;
        int s = pickRoute(u, -1, v);
        if (s < 0) return false;
        int L = len[s];
        if (L == 0) return false;
        const int* R = r[s];
        int x1 = (v >= 0 && rng.below(2)) ? pos[v] : 1 + rng.below(L);
        int q = rng.below(10);
        int k = q < 7 ? 1 : (q < 9 ? 2 : 3);
        if (x1 + k - 1 > L) k = L - x1 + 1;
        int x2 = x1 + k - 1, rp;
        int rd = removal(R, x1, x2, rp);
        int p, rev;
        int nt = tm[s] + rd + scan(R, L, u, u, x1, x2, p, rev) + svc[u];
        if (nt > MAXT) return false;
        int delta = prof[u] - rp - (nt - tm[s]);
        if (!acc(delta)) return false;
        int rem[3];
        for (int i = 0; i < k; i++) rem[i] = R[x1 + i];
        int n = build(R, L, x1, x2, p, &u, 1, buf1);
        unRemove(u);
        commit(s, buf1, n, nt, pf[s] - rp + prof[u]);
        for (int i = 0; i < k; i++) unAdd(rem[i]);
        fin(delta);
        return true;
    }
    // remove 1-2 consecutive orders, insert two unassigned orders
    bool mvReplace2() {
        if (nun < 2) return false;
        int u1 = pickUn(), v;
        int s = pickRoute(u1, -1, v);
        if (s < 0) return false;
        int L = len[s];
        if (L == 0 || L >= MAXL - 4) return false;
        const int* R = r[s];
        int x1 = (v >= 0 && rng.below(2)) ? pos[v] : 1 + rng.below(L);
        int k = rng.below(4) == 0 ? 2 : 1;
        if (x1 + k - 1 > L) k = L - x1 + 1;
        int x2 = x1 + k - 1, rp;
        int rd = removal(R, x1, x2, rp);
        int p, rev;
        int t1 = tm[s] + rd + scan(R, L, u1, u1, x1, x2, p, rev) + svc[u1];
        if (t1 > MAXT) return false;
        int u2 = -1;
        for (int t = 0; t < 5; t++) {
            int w = (t < 3) ? NN[u1][rng.below(KNN)] : un[rng.below(nun)];
            if (stu[w] < 0 && !dead[w] && w != u1 && el(w, s)) { u2 = w; break; }
        }
        if (u2 < 0) return false;
        int n1 = build(R, L, x1, x2, p, &u1, 1, buf1);
        int p2;
        int nt = t1 + scan(buf1, n1, u2, u2, BIG, BIG, p2, rev) + svc[u2];
        if (nt > MAXT) return false;
        int delta = prof[u1] + prof[u2] - rp - (nt - tm[s]);
        if (!acc(delta)) return false;
        int rem[2];
        for (int i = 0; i < k; i++) rem[i] = R[x1 + i];
        int n = build(buf1, n1, BIG, BIG, p2, &u2, 1, buf2);
        unRemove(u1); unRemove(u2);
        commit(s, buf2, n, nt, pf[s] - rp + prof[u1] + prof[u2]);
        for (int i = 0; i < k; i++) unAdd(rem[i]);
        fin(delta);
        return true;
    }
    bool mvRelocate() {
        int a = rng.below(N), s1 = stu[a];
        if (s1 < 0) return false;
        int v, s2 = pickRoute(a, -1, v);
        if (s2 < 0) return false;
        int x = pos[a], p, rev;
        const int* R1 = r[s1];
        int g = (int)D[R1[x - 1]][R1[x + 1]] - (int)D[R1[x - 1]][a] - (int)D[a][R1[x + 1]];
        if (s2 == s1) {
            int ins = scan(R1, len[s1], a, a, x, x, p, rev);
            if (p == x - 1) return false;
            int d = g + ins;
            if (tm[s1] + d > MAXT) return false;
            if (!acc(-d)) return false;
            int n = build(R1, len[s1], x, x, p, &a, 1, buf1);
            commit(s1, buf1, n, tm[s1] + d, pf[s1]);
            fin(-d);
            return true;
        }
        if (len[s2] >= MAXL - 3) return false;
        int nt2 = tm[s2] + scan(r[s2], len[s2], a, a, BIG, BIG, p, rev) + svc[a];
        if (nt2 > MAXT) return false;
        int nt1 = tm[s1] + g - svc[a];
        if (nt1 > MAXT) return false;
        int delta = -(nt1 - tm[s1]) - (nt2 - tm[s2]);
        if (!acc(delta)) return false;
        int n1 = build(R1, len[s1], x, x, -1, nullptr, 0, buf1);
        int n2 = build(r[s2], len[s2], BIG, BIG, p, &a, 1, buf2);
        commit(s1, buf1, n1, nt1, pf[s1] - prof[a]);
        commit(s2, buf2, n2, nt2, pf[s2] + prof[a]);
        fin(delta);
        return true;
    }
    // exchange two orders between two routes, each re-inserted at its best position
    bool mvSwap() {
        int a = rng.below(N), s1 = stu[a];
        if (s1 < 0) return false;
        int b = -1, s2 = -1;
        for (int t = 0; t < 3; t++) {
            int w = NN[a][rng.below(KNN)];
            int s = stu[w];
            if (s >= 0 && s != s1 && el(a, s)) { s2 = s; b = w; break; }
        }
        if (s2 < 0) {
            s2 = elig[a][rng.below((int)elig[a].size())];
            if (s2 == s1 || len[s2] == 0) return false;
            b = r[s2][1 + rng.below(len[s2])];
        } else if (rng.below(2)) b = r[s2][1 + rng.below(len[s2])];
        if (!el(b, s1)) return false;
        int x = pos[a], y = pos[b], p1, p2, rev, rp;
        const int *R1 = r[s1], *R2 = r[s2];
        int nt1 = tm[s1] + removal(R1, x, x, rp) + scan(R1, len[s1], b, b, x, x, p1, rev) + svc[b];
        if (nt1 > MAXT) return false;
        int nt2 = tm[s2] + removal(R2, y, y, rp) + scan(R2, len[s2], a, a, y, y, p2, rev) + svc[a];
        if (nt2 > MAXT) return false;
        int delta = -(nt1 - tm[s1]) - (nt2 - tm[s2]);
        if (!acc(delta)) return false;
        int n1 = build(R1, len[s1], x, x, p1, &b, 1, buf1);
        int n2 = build(R2, len[s2], y, y, p2, &a, 1, buf2);
        commit(s1, buf1, n1, nt1, pf[s1] - prof[a] + prof[b]);
        commit(s2, buf2, n2, nt2, pf[s2] - prof[b] + prof[a]);
        fin(delta);
        return true;
    }
    bool mvTwoOpt() {
        int a = rng.below(N), s = stu[a];
        if (s < 0) return false;
        int L = len[s];
        if (L < 2) return false;
        int* R = r[s];
        int i, j;
        int v = -1;
        for (int t = 0; t < 2; t++) { int w = NN[a][rng.below(KNN)]; if (stu[w] == s) { v = w; break; } }
        if (v >= 0) {
            int lo = std::min(pos[a], pos[v]), hi = std::max(pos[a], pos[v]);
            if (rng.below(2)) { i = lo; j = hi; } else { i = lo - 1; j = hi - 1; }
        } else {
            i = rng.below(L); j = i + 1 + rng.below(L - i);
        }
        if (j < i + 2) return false;
        int d = (int)D[R[i]][R[j]] + (int)D[R[i + 1]][R[j + 1]] - (int)D[R[i]][R[i + 1]] - (int)D[R[j]][R[j + 1]];
        if (tm[s] + d > MAXT) return false;
        if (!acc(-d)) return false;
        std::reverse(R + i + 1, R + j + 1);
        for (int q = i + 1; q <= j; q++) pos[R[q]] = q;
        tm[s] += d;
        fin(-d);
        return true;
    }
    // move a segment of 2-3 consecutive orders (possibly reversed) within or between routes
    bool mvOrOpt() {
        int a = rng.below(N), s1 = stu[a];
        if (s1 < 0) return false;
        int L1 = len[s1], x1 = pos[a];
        int k = 2 + rng.below(2);
        int x2 = x1 + k - 1;
        if (x2 > L1) return false;
        const int* R1 = r[s1];
        int f = R1[x1], l = R1[x2];
        int v, s2 = pickRoute(a, -1, v);
        if (s2 < 0) return false;
        int seg[3], p, rev;
        if (s2 == s1) {
            int g = (int)D[R1[x1 - 1]][R1[x2 + 1]] - (int)D[R1[x1 - 1]][f] - (int)D[l][R1[x2 + 1]];
            int ins = scan(R1, L1, f, l, x1, x2, p, rev);
            if (p == x1 - 1 && !rev) return false;
            int d = g + ins;
            if (tm[s1] + d > MAXT) return false;
            if (!acc(-d)) return false;
            for (int i = 0; i < k; i++) seg[i] = rev ? R1[x2 - i] : R1[x1 + i];
            int n = build(R1, L1, x1, x2, p, seg, k, buf1);
            commit(s1, buf1, n, tm[s1] + d, pf[s1]);
            fin(-d);
            return true;
        }
        if (len[s2] >= MAXL - 5) return false;
        for (int i = x1; i <= x2; i++) if (!el(R1[i], s2)) return false;
        int rp;
        int rd = removal(R1, x1, x2, rp);
        int nt1 = tm[s1] + rd;
        if (nt1 > MAXT) return false;
        int inner = 0, sv = 0;
        for (int i = x1; i <= x2; i++) { sv += svc[R1[i]]; if (i < x2) inner += D[R1[i]][R1[i + 1]]; }
        int nt2 = tm[s2] + scan(r[s2], len[s2], f, l, BIG, BIG, p, rev) + inner + sv;
        if (nt2 > MAXT) return false;
        int delta = -(nt1 - tm[s1]) - (nt2 - tm[s2]);
        if (!acc(delta)) return false;
        for (int i = 0; i < k; i++) seg[i] = rev ? R1[x2 - i] : R1[x1 + i];
        int n1 = build(R1, L1, x1, x2, -1, nullptr, 0, buf1);
        int n2 = build(r[s2], len[s2], BIG, BIG, p, seg, k, buf2);
        commit(s1, buf1, n1, nt1, pf[s1] - rp);
        commit(s2, buf2, n2, nt2, pf[s2] + rp);
        fin(delta);
        return true;
    }
    // ejection chain: unassigned u enters route s1 and pushes order a into route s2;
    // optionally a in turn pushes order b out of s2 (b becomes unassigned)
    bool mvEject() {
        if (!nun) return false;
        int u = pickUn(), v;
        int s1 = pickRoute(u, -1, v);
        if (s1 < 0 || len[s1] == 0) return false;
        const int* R1 = r[s1];
        int x = (v >= 0 && rng.below(2)) ? pos[v] : 1 + rng.below(len[s1]);
        int a = R1[x], v2;
        int s2 = pickRoute(a, s1, v2);
        if (s2 < 0 || len[s2] >= MAXL - 3) return false;
        int p1, p2, rev, rp;
        int nt1 = tm[s1] + removal(R1, x, x, rp) + scan(R1, len[s1], u, u, x, x, p1, rev) + svc[u];
        if (nt1 > MAXT) return false;
        const int* R2 = r[s2];
        int y = BIG, b = -1, nt2, bp = 0;
        if (len[s2] > 0 && rng.below(5) < 2) {
            y = (v2 >= 0 && rng.below(2)) ? pos[v2] : 1 + rng.below(len[s2]);
            b = R2[y];
            nt2 = tm[s2] + removal(R2, y, y, bp) + scan(R2, len[s2], a, a, y, y, p2, rev) + svc[a];
        } else {
            nt2 = tm[s2] + scan(R2, len[s2], a, a, BIG, BIG, p2, rev) + svc[a];
        }
        if (nt2 > MAXT) return false;
        int delta = prof[u] - bp - (nt1 - tm[s1]) - (nt2 - tm[s2]);
        if (!acc(delta)) return false;
        int n1 = build(R1, len[s1], x, x, p1, &u, 1, buf1);
        int n2 = build(R2, len[s2], y, y, p2, &a, 1, buf2);
        unRemove(u);
        commit(s1, buf1, n1, nt1, pf[s1] - prof[a] + prof[u]);
        commit(s2, buf2, n2, nt2, pf[s2] + prof[a] - bp);
        if (b >= 0) unAdd(b);
        fin(delta);
        return true;
    }

    // ---------- ruin & recreate (SISR-style): remove strings around a seed from a few routes,
    // then greedily re-insert removed + nearby unassigned orders at their cheapest feasible position
    int sv_r[S][MAXL + 4], sv_len[S], sv_tm[S], sv_pf[S], sv_list[S], nsaved = 0;
    bool saved[S] = {false};
    int cand[MAXN], ncand = 0, stamp[MAXN] = {0}, cur_stamp = 0;
    bool corig[MAXN];
    inline void saveRoute(int s) {
        saved[s] = true; sv_list[nsaved++] = s;
        sv_len[s] = len[s]; sv_tm[s] = tm[s]; sv_pf[s] = pf[s];
        memcpy(sv_r[s], r[s], sizeof(int) * (len[s] + 2));
    }
    inline void eraseRange(int s, int x1, int x2) {
        int* a = r[s]; int L = len[s], k = x2 - x1 + 1, rp;
        int d = removal(a, x1, x2, rp);
        for (int i = x2 + 1; i <= L + 1; i++) a[i - k] = a[i];
        for (int i = x1; i <= L - k; i++) pos[a[i]] = i;
        len[s] = L - k; tm[s] += d; pf[s] -= rp; sc += -rp - d;
    }
    inline void insertAfter(int s, int p, int o, int dtime) {
        int* a = r[s]; int L = len[s];
        for (int i = L + 1; i > p; i--) a[i + 1] = a[i];
        a[p + 1] = o;
        for (int i = p + 1; i <= L + 1; i++) pos[a[i]] = i;
        stu[o] = s; len[s] = L + 1; tm[s] += dtime; pf[s] += prof[o]; sc += prof[o] - dtime;
    }
    bool mvRuin() {
        int seed = rng.below(N);
        if (dead[seed]) return false;
        long long sc0 = sc;
        nsaved = 0; ncand = 0; cur_stamp++;
        int nr = 1 + rng.below(3), maxUn = 1 + rng.below(6), ruined = 0, unc = 0;
        for (int q = -1; q < KB && (ruined < nr || q < 10); q++) {
            int w = q < 0 ? seed : NNB[seed][q];
            int s = stu[w];
            if (s >= 0) {
                if (ruined < nr && !saved[s]) {
                    saveRoute(s);
                    int L = len[s], l = 1 + rng.below(std::min(L, 10)), x = pos[w];
                    int lo = std::max(1, x - l + 1), hi = std::min(x, L - l + 1);
                    int x1 = lo + rng.below(hi - lo + 1);
                    int tmp[10];
                    for (int i = 0; i < l; i++) tmp[i] = r[s][x1 + i];
                    eraseRange(s, x1, x1 + l - 1);
                    for (int i = 0; i < l; i++) { int o = tmp[i]; cand[ncand++] = o; corig[o] = false; stamp[o] = cur_stamp; unAdd(o); }
                    ruined++;
                }
            } else if (!dead[w] && stamp[w] != cur_stamp && unc < maxUn) {
                cand[ncand++] = w; corig[w] = true; stamp[w] = cur_stamp; unc++;
            }
        }
        // insertion order: random / most valuable per minute first / most profit first
        {
            int mode = rng.below(10);
            float key[64 + 40];
            int idx[64 + 40];
            for (int i = 0; i < ncand; i++) {
                idx[i] = i;
                key[i] = mode < 4 ? (float)rng.uni() : (mode < 8 ? -dens[cand[i]] * (0.9f + 0.2f * (float)rng.uni()) : -(float)prof[cand[i]]);
            }
            std::sort(idx, idx + ncand, [&](int a, int b) { return key[a] < key[b]; });
            int tmpc[64 + 40];
            for (int i = 0; i < ncand; i++) tmpc[i] = cand[idx[i]];
            memcpy(cand, tmpc, sizeof(int) * ncand);
        }
        for (int i = 0; i < ncand; i++) {
            int c = cand[i], best = INT_MAX, bs = -1, bp = 0, p, rev;
            for (int s : elig[c]) {
                if (tm[s] + svc[c] > MAXT || len[s] >= MAXL - 3) continue;
                if (rng.below(100) < 2) continue;                       // blink
                int d = scan(r[s], len[s], c, c, BIG, BIG, p, rev) + svc[c];
                if (tm[s] + d > MAXT) continue;
                if (d < best) { best = d; bs = s; bp = p; }
            }
            if (bs >= 0 && prof[c] - best > 0) {
                if (!saved[bs]) saveRoute(bs);
                if (!dead[c]) unRemove(c);
                insertAfter(bs, bp, c, best);
            }
        }
        int delta = (int)(sc - sc0);
        if (acc(delta)) {
            for (int i = 0; i < nsaved; i++) saved[sv_list[i]] = false;
            if (sc > bsc) saveBest();
            return true;
        }
        // roll back
        for (int i = 0; i < ncand; i++) {
            int c = cand[i];
            bool nowUn = stu[c] < 0;
            if (nowUn && !corig[c] && !dead[c]) unRemove(c);
            corig[c] = corig[c] && !nowUn;          // true -> must go back to the unassigned list
        }
        for (int i = 0; i < nsaved; i++) {
            int s = sv_list[i];
            saved[s] = false;
            commit(s, sv_r[s], sv_len[s], sv_tm[s], sv_pf[s]);
        }
        for (int i = 0; i < ncand; i++) if (corig[cand[i]]) unAdd(cand[i]);
        sc = sc0;
        return false;
    }

    // ---------- driver
    void init(uint64_t seed) {
        rng = Rng(seed);
        memset(opA, 0, sizeof(opA)); memset(opS, 0, sizeof(opS));
        double tot = 0, c = 0;
        for (int i = 0; i < NOP; i++) tot += op_w[i];
        for (int i = 0; i < NOP; i++) { c += op_w[i] / tot; op_thr[i] = (uint32_t)std::min(4294967295.0, c * 4294967296.0); }
        op_thr[NOP - 1] = 0xFFFFFFFFu;
        int ll[S] = {0};
        static const int empty[S][MAXL] = {};
        loadRoutes(empty, ll);
        bsc = LLONG_MIN;
        saveBest();
    }
    inline void step() {
        uint32_t x = (uint32_t)(rng.next() >> 32);
        int op = 0;
        while (x > op_thr[op]) op++;
        bool ok;
        switch (op) {
            case ADD:      ok = mvAdd(); break;
            case REMOVE:   ok = mvRemove(); break;
            case REPLACE:  ok = mvReplace(); break;
            case REPLACE2: ok = mvReplace2(); break;
            case RELOCATE: ok = mvRelocate(); break;
            case SWAP:     ok = mvSwap(); break;
            case TWOOPT:   ok = mvTwoOpt(); break;
            case OROPT:    ok = mvOrOpt(); break;
            case EJECT:    ok = mvEject(); break;
            default:       ok = mvRuin(); break;
        }
        opA[op]++; opS[op] += ok;
    }
    // geometric cooling from t0 to t1 (euro) over `seconds` of wall time
    void anneal(double t0, double t1, double seconds) {
        auto st = std::chrono::steady_clock::now();
        double lr = std::log(t1 / t0); double next_tr = 0;
        setT(t0);
        for (;;) {
            for (int i = 0; i < 16384; i++) step();
            iters += 16384;
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - st).count();
            if (el >= seconds) break;
            setT(t0 * std::exp(lr * el / seconds));
            if (trace && el >= next_tr) { next_tr += seconds / 40; int tt=0; for(int s=0;s<S;s++) tt+=tm[s]; long long sv=0; for(int s=0;s<S;s++) for(int i=1;i<=len[s];i++) sv+=svc[r[s][i]]; printf("  T=%.4f cur=%.2f best=%.2f travel=%.0fmin slack=%.0fmin nun=%d\n", T/60, sc/60.0, bsc/60.0, (tt-sv)/60.0, (S*MAXT-tt)/60.0, nun); }
        }
    }
    // full recomputation; returns false when the incremental bookkeeping disagrees
    bool selfCheck() const {
        long long tot = 0;
        std::vector<char> seen(N, 0);
        for (int s = 0; s < S; s++) {
            int t = 0, p = 0, prev = DEP;
            for (int i = 1; i <= len[s]; i++) {
                int o = r[s][i];
                if (seen[o] || !el(o, s) || stu[o] != s || pos[o] != i) return false;
                seen[o] = 1;
                t += D[prev][o] + svc[o]; p += prof[o]; prev = o;
            }
            t += D[prev][DEP];
            if (t != tm[s] || p != pf[s] || t > MAXT) return false;
            tot += p - t;
        }
        int cnt = 0;
        for (int i = 0; i < N; i++) if (!seen[i]) { if (stu[i] != -1) return false; if (dead[i]) continue; cnt++; if (un[upos[i]] != i) return false; }
        return tot == sc && cnt == nun;
    }
};

static void publish(Solver& sv) {
    std::lock_guard<std::mutex> g(shared.mu);
    if (sv.bsc > shared.sc) {
        shared.sc = sv.bsc;
        for (int s = 0; s < S; s++) { shared.len[s] = sv.blen[s]; memcpy(shared.r[s], sv.br[s], sizeof(int) * sv.blen[s]); }
    }
}
static void adopt(Solver& sv) {
    std::lock_guard<std::mutex> g(shared.mu);
    if (shared.sc > sv.bsc) {
        sv.bsc = shared.sc;
        for (int s = 0; s < S; s++) { sv.blen[s] = shared.len[s]; memcpy(sv.br[s], shared.r[s], sizeof(int) * shared.len[s]); }
    }
}

static std::mutex print_mu;
static void worker(int tid, uint64_t seed, double budget, bool warm, Solver* sv) {
    auto st = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - st).count(); };
    sv->init(seed); sv->trace = (tid == 0 && getenv("SA_TRACE"));
    if (warm) { adopt(*sv); sv->loadBest(); }
    else {
        sv->anneal(P_T0, P_T1, budget * P_FRAC1);
        publish(*sv);
        std::lock_guard<std::mutex> g(print_mu);
        printf("[t%d] main anneal done  %7.1fs  best=%.2f\n", tid, elapsed(), sv->bsc / 60.0);
        fflush(stdout);
    }
    int cyc = 0;
    while (true) {
        double left = budget - elapsed();
        if (left < 1.0) break;
        adopt(*sv);
        sv->loadBest();
        // alternate reheat strength so the threads explore at different scales
        double tr = P_TR * (((cyc + tid) & 1) ? 0.6 : 1.0);
        sv->anneal(tr, P_T1, std::min(P_CYCLE, left));
        publish(*sv);
        cyc++;
        std::lock_guard<std::mutex> g(print_mu);
        printf("[t%d] reheat %3d (T=%.2f) %7.1fs  best=%.2f\n", tid, cyc, tr, elapsed(), sv->bsc / 60.0);
        fflush(stdout);
    }
    if (!sv->selfCheck()) { std::lock_guard<std::mutex> g(print_mu); printf("[t%d] WARNING: self-check failed\n", tid); }
}

// [Phase 2 fix] the warm-start file is parsed strictly: every token must be an integer, route lengths
// must fit the route arrays (previously a length > MAXL wrote past rr[][] / Solver::r[][]), and every
// rejection is reported on stderr; the run then continues from scratch.
static bool load_solution(const char* fn) {
    std::ifstream f(fn);
    if (!f) { std::cerr << "cannot open warm start file " << fn << ", ignoring it\n"; return false; }
    auto bad = [&](const std::string& msg) { std::cerr << "malformed warm start file " << fn << ": " << msg << ", ignoring it\n"; return false; };
    std::string tok;
    long long v;
    if (!(f >> tok) || !parse_int(tok, LLONG_MIN, LLONG_MAX, v)) return bad("first line must be the claimed profit");
    int ll[S];
    static int rr[S][MAXL];
    const int max_route = MAXL - 3;                 // largest route length the moves can produce
    for (int s = 0; s < S; s++) {
        if (!(f >> tok) || !parse_int(tok, LLONG_MIN, LLONG_MAX, v))
            return bad("missing or non-integer route length for consultant " + std::to_string(s + 1));
        if (v < 0 || v > max_route) {
            std::cerr << "warm start file " << fn << ": route length " << v << " of consultant " << s + 1
                      << " is outside 0.." << max_route << ", ignoring it\n";
            return false;
        }
        ll[s] = (int)v;
        for (int i = 0; i < ll[s]; i++) {
            if (!(f >> tok) || !parse_int(tok, INT_MIN, INT_MAX, v))
                return bad("missing or non-integer order id for consultant " + std::to_string(s + 1));
            auto it = oid_to_idx.find((int)v);
            if (it == oid_to_idx.end()) { std::cerr << "unknown order " << v << "\n"; return false; }
            rr[s][i] = it->second;
        }
    }
    Solver* sv = new Solver();
    sv->rng = Rng(1);
    sv->loadRoutes(rr, ll);
    if (!sv->selfCheck()) { std::cerr << "warm start solution is not feasible, ignoring it\n"; delete sv; return false; }
    sv->bsc = LLONG_MIN; sv->saveBest();
    publish(*sv);
    printf("Warm start from %s: %.2f\n", fn, sv->bsc / 60.0);
    delete sv;
    return true;
}

static void write_output(const char* fn) {
    // independent evaluation of the shared best, straight from the node matrix
    long long tot_p = 0, tot_t = 0;
    for (int s = 0; s < S; s++) {
        int cur = HQ_NODE, t = 0;
        for (int i = 0; i < shared.len[s]; i++) {
            int o = shared.r[s][i];
            t += dtn[cur][onode[o]] + svc[o]; tot_p += prof[o]; cur = onode[o];
        }
        t += dtn[cur][HQ_NODE];
        tot_t += t;
        if (t > MAXT) printf("ERROR: student %d works overtime (%d s)\n", s + 1, t);
    }
    long long net = tot_p - tot_t;                    // 1/60 euro
    long long fl = net >= 0 ? net / 60 : -((-net + 59) / 60);
    printf("\nTotal profit: %lld  total time cost: %.2f  net profit: %.2f\n", tot_p / 60, tot_t / 60.0, net / 60.0);
    std::ofstream f(fn);
    f << fl << "\n";
    for (int s = 0; s < S; s++) {
        f << shared.len[s] << "\n";
        for (int i = 0; i < shared.len[s]; i++) f << oid[shared.r[s][i]] << "\n";
    }
}

int main(int argc, char** argv) {
    double budget = argc > 1 ? atof(argv[1]) : 80.0;
    const char* warm_file = (argc > 2 && argv[2][0]) ? argv[2] : nullptr;
    uint64_t seed = argc > 3 ? strtoull(argv[3], nullptr, 10) : (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    if (const char* e = getenv("SA_T0")) P_T0 = atof(e);
    if (const char* e = getenv("SA_T1")) P_T1 = atof(e);
    if (const char* e = getenv("SA_TR")) P_TR = atof(e);
    if (const char* e = getenv("SA_FRAC1")) P_FRAC1 = atof(e);
    if (const char* e = getenv("SA_CYCLE")) P_CYCLE = atof(e);
    if (const char* e = getenv("SA_THREADS")) P_THREADS = atoi(e);
    if (const char* e = getenv("SA_TOUR")) P_TOUR = atoi(e);
    if (const char* e = getenv("SA_W")) { std::istringstream ss(e); for (int i = 0; i < NOP; i++) ss >> op_w[i]; }

    // [staging change] input directory is configurable via SA_DATA_DIR (default: current directory)
    const std::string data_dir = getenv("SA_DATA_DIR") ? getenv("SA_DATA_DIR") : ".";
    read_orders((data_dir + "/orders.txt").c_str());
    read_times((data_dir + "/drivingtimes.txt").c_str());
    check_instance((data_dir + "/orders.txt").c_str(), (data_dir + "/drivingtimes.txt").c_str());
    precompute();
    int nthreads = P_THREADS > 0 ? P_THREADS : std::max(1u, std::thread::hardware_concurrency());
    printf("Loaded %d orders, %dx%d driving times. %d thread(s), %.0f s budget.\n", N, num_nodes, num_nodes, nthreads, budget);

    bool warm = warm_file && load_solution(warm_file);
    std::vector<Solver*> solvers;
    std::vector<std::thread> th;
    for (int t = 0; t < nthreads; t++) solvers.push_back(new Solver());
    for (int t = 0; t < nthreads; t++)
        th.emplace_back(worker, t, seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL * (t + 1), budget, warm, solvers[t]);
    for (auto& x : th) x.join();

    long long its = 0, a[NOP] = {0}, sc[NOP] = {0};
    for (auto* sv : solvers) { its += sv->iters; for (int i = 0; i < NOP; i++) { a[i] += sv->opA[i]; sc[i] += sv->opS[i]; } }
    printf("\n=== DONE: %.0fM iterations (%.2fM it/s per thread), best=%.2f ===\n", its / 1e6, its / 1e6 / budget / nthreads, shared.sc / 60.0);
    printf("Operator stats:\n");
    for (int i = 0; i < NOP; i++) printf("  %-13s attempts=%-12lld accepted=%-11lld rate=%.4f\n", op_names[i], a[i], sc[i], a[i] ? (double)sc[i] / a[i] : 0.0);
    write_output("output.txt");
    return 0;
}
