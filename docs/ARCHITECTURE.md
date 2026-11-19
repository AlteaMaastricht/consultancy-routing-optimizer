# Architecture

The solver is `cpp/sa.cpp` (C++17, multithreaded, single file). Python tooling (standard library
only) generates instances, validates solutions and drives the tests:

```
cpp/sa.cpp                  simulated-annealing solver
tools/make_instance.py      synthetic instance generator (course file formats)
tools/validate_solution.py  independent validator used by every test and for all reported results
tests/                      unittest: C++ runs + input validation, validator/generator
```

## Problem model
- Orders `o` with a node, a service duration `d_o` (minutes), a profit `p_o` (EUR) and a set of
  eligible consultants.
- 20 consultants. Each route starts and ends at depot node 251. Shift = driving + service ≤ 8 h.
- Objective: maximise `Σ p_o − (driving + service seconds) / 60`, i.e. EUR 60 per hour.
- Orders may stay unserved. This makes it a *team orienteering problem* with eligibility
  constraints, not a classic VRP.

## Data layout (read-only after loading)
```
oid/onode/svc/prof[MAXN=1280]  order id, node, service seconds, profit in 1/60 EUR (= profit*60)
emask[o]  (uint32 bitmask)     eligible consultants; elig[o] = same as a list
D[MAXN][MAXN] (uint16)         order-to-order driving seconds; index N = depot (node 251)
NN[o][24], NNB[o][48]          nearest orders (small moves / ruin & recreate)
dead[o]                        profit <= service cost: never inserted (see Limitations)
dens[o]                        profit per service second (biases sampling of unassigned orders)
```
Objective: integer `sc = Σ_s (pf[s] − tm[s])` in 1/60 EUR, i.e. `60·profit − seconds`. Net
profit = `sc / 60` EUR. There is no floating-point drift.

## Per-thread solver state (`struct Solver`)
- Each route is stored as `r[s][0..len+1]` with the depot as a sentinel at both ends, so there are
  no first- or last-position special cases.
- `tm[s]` and `pf[s]` are the route time and profit, and `stu[o]` and `pos[o]` give each order's
  consultant and position.
- Unassigned, non-dead orders live in a swap-remove array (`un`, `upos`).
- **Every move keeps every route feasible:** each candidate is rejected unless `tm ≤ 28,800`.

## Moves (default weights `op_w`, environment-tunable through `SA_W`)
| Move | Weight | Description |
|---|---|---|
| add | 8 | unassigned order → route of a nearest neighbour (80 %) or a random eligible consultant, best insertion position |
| remove | 2 | drop an order (only if its removal does not increase time) |
| replace(k-1) | 20 | remove 1–3 consecutive orders, insert one unassigned order at its best position |
| replace(k-2) | 6 | remove 1–2 consecutive orders, insert two unassigned orders |
| relocate | 18 | move an order to its best position in the same or another route |
| swap | 16 | exchange two orders between routes, each re-inserted at its best position |
| 2-opt | 8 | neighbour-guided segment reversal (O(1) delta; assumes a symmetric matrix) |
| or-opt | 8 | move 2–3 consecutive orders, optionally reversed, within or between routes |
| eject-chain | 14 | unassigned `u` enters route s1 and pushes order `a` into route s2, optionally pushing `b` out of s2 |
| ruin-recreate | 0.5 | SISR-style: cut strings around a seed from 1–3 nearby routes, add up to 6 nearby unassigned orders, then greedily re-insert all candidates (random, density or profit order, 2 % "blinks"). Rolled back if rejected. |

Notes on the moves:
- **Insertion cost:** best insertion is an O(route length) scan (`scan`). All other deltas are O(1)
  or O(segment).
- **Acceptance:** Metropolis, `exp(Δ / T)` with T in EUR × 60. A move is rejected without
  sampling when `Δ / T < −30`.

## Schedule and threads
```
main ─ read data ─ precompute ─ optional warm start (validated by selfCheck) ─┐
                                                                             ▼
 thread t (seed = f(seed, t)):  main anneal T 5 → 0.04 EUR, geometric in wall-clock time,
                                75 % of the budget (SA_FRAC1)
                                └─ loop: adopt shared best → reheat to T_R (0.6 or 0.36 EUR,
                                   alternating per thread and cycle) → cool to 0.04 over 20 s
                                   (SA_CYCLE) → publish
 shared best: mutex-protected (publish / adopt); output written after join
 write_output: re-evaluates the shared best from the node matrix and writes output.txt (floor of EUR)
```
- `selfCheck()` (a full recomputation of times, profits, eligibility, duplicates and the unassigned
  list) runs once per thread at the end, and once on warm start.
- RNG: xorshift64 for each thread. The seed comes from `argv[3]`, or from the clock if absent.

## Environment knobs
`SA_T0`, `SA_T1`, `SA_TR`, `SA_FRAC1`, `SA_CYCLE`, `SA_THREADS`, `SA_TOUR`, `SA_W`, `SA_TRACE`,
plus `SA_DATA_DIR` (added in staging of the C++ solver). Command line: `sa [seconds=80] [warm_start_file] [seed]`.

## Input validation (Phase 2)
`read_orders`, `read_times`, `check_instance` and `load_solution` parse every field strictly and
check these limits:
- array capacities (1,279 orders, 1,200 nodes);
- 0/1 eligibility flags;
- driving times from 0 to 65,535 s, because they are stored as `uint16`;
- a square matrix;
- depot node 251 present;
- order nodes inside the matrix;
- warm-start route lengths from 0 to `MAXL-3`.

Invalid instance files stop with `file:line: message` (exit 1). An invalid warm-start file is
reported and ignored.
