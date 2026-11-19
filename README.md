# Consultancy Routing Optimizer

Simulated annealing that decides which consulting orders to accept and how to route 20
consultants through them, maximising profit minus labour cost. The solver is a single-file,
multithreaded C++17 program; its solutions are checked by an independent validator.

## 1. Overview
- **`cpp/sa.cpp`:** a single-file C++17 solver. It uses an exact integer objective, ten
  move types (including ejection chains and SISR-style ruin-and-recreate), best-position insertion,
  and a wall-clock annealing schedule. Several threads share the best solution, and a run can be
  warm-started from a previous solution.
- **Tooling (Python, standard library only):** an independent solution validator, a synthetic
  instance generator, and a test suite that builds on both to exercise the C++ binary.

On the course-provided instance (not distributed), the best verified solution has a net profit of
**EUR 9,600.77**, from the C++ solver. A 25-second run of the current C++ code on two threads
reached **EUR 9,473.45**. Both are heuristic results: no optimum or bound is known.

## 2. Problem
Profit-maximising team orienteering with skill eligibility:
- about 1,177 candidate orders, each with a node, a service time in minutes, a profit in EUR and
  0/1 eligibility flags for 20 consultants;
- every route starts and ends at the depot (node 251) and takes at most 8 h of driving plus service;
- labour costs EUR 60/h, and driving times are a symmetric matrix in seconds;
- the goal is to choose a subset of orders, assign it to eligible consultants and sequence the
  routes to maximise `Σ profit − EUR 60/h × (driving + service time)`. Unprofitable orders can stay
  unserved.

## 3. Approach
The solver keeps every route feasible at all times and uses Metropolis acceptance.

| | |
|---|---|
| Moves | add, remove, replace k-out/1-in and k-out/2-in, relocate, swap with best re-insertion, NN-guided 2-opt, or-opt with reversal, ejection chains, SISR ruin-and-recreate with rollback |
| Insertion | best position, kNN-guided target route |
| Objective | integer `60·profit − seconds` (exact, no floating-point drift) |
| Schedule | geometric in wall-clock time (5 → 0.04 EUR over 75 % of the budget), then 20 s reheat cycles from the shared best |
| Parallelism | threads with a mutex-protected shared best |

Details are in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## 4. Architecture
```
cpp/sa.cpp                  C++17 solver
tools/validate_solution.py  independent validator (feasibility + objective)
tools/make_instance.py      synthetic instance generator (same file formats)
tests/                      unittest: validator, generator, C++ runs + input validation
results/                    verified scores on the course instance + provenance
docs/                       ARCHITECTURE.md, PROVENANCE.md
```

## 5. Technologies
- C++17 using the STL and `std::thread`, with no external libraries. Built with CMake ≥ 3.16.
- Python 3 (standard library only) for the validator, the instance generator and the tests.
- Validated with Apple clang 14 (x86_64), CMake 4.4 and Python 3.14.0.

## 6. Installation & running
```sh
cmake -S . -B build && cmake --build build                  # -> build/sa (portable)
cmake -S . -B build-native -DSA_NATIVE=ON && cmake --build build-native   # optional -march=native
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSA_SANITIZE=ON && cmake --build build-asan   # ASan + UBSan
```
**Input:**
- `orders.txt`: tab-separated, one header line, columns
  `Order, City, Node ID, X, Y, Duration (min), Profit (eur), Student 1..20`;
- `drivingtimes.txt`: a square integer matrix in seconds.

The course instance is not included. Generate a synthetic instance instead:
```sh
python3 tools/make_instance.py --out-dir data/demo --orders 400 --nodes 400 --seed 7
```
**Solver limits:**
- the solver assumes 20 consultants and depot node 251, so instances need more than 251 nodes;
- it accepts 1 to 1,279 orders, at most 1,200 nodes and driving times of at most
  65,535 s. It rejects any other input with `file:line: message`.

## 7. Example usage
```sh
# C++: sa [seconds=80] [warm_start_file] [seed]; reads $SA_DATA_DIR (default: current dir), writes ./output.txt
cd data/demo && SA_THREADS=2 ../../build/sa 10 "" 42 && cd ../..
python3 tools/validate_solution.py --orders data/demo/orders.txt --times data/demo/drivingtimes.txt data/demo/output.txt
SA_THREADS=2 SA_DATA_DIR=data/demo build/sa 10 data/demo/output.txt 7      # warm start

sh examples/run_demo.sh                                   # generate, solve, validate
SA_BIN=build/sa python3 -m unittest discover -s tests     # all tests
```
**Output:** the first line is the net profit (integer EUR, rounded down). Then, for each consultant,
a line with `k` followed by `k` order IDs in visiting order.

## 8. Results
All values below are **on the course-provided instance, not distributed**. They are heuristic
results, with no optimality claim. Each was recomputed by `tools/validate_solution.py`, which checks
duplicates, eligibility, the 8 h limit including driving and service, depot start and end, and the
objective. Each was also cross-checked with the course checker. The solution files themselves
(order IDs per route) are **not included**, because they derive from the course instance; they will
only be shared if the course coordinator agrees. See [`results/README.md`](results/README.md).

| Run (solution file not distributed) | Net profit (EUR) | Orders | Implementation and conditions |
|---|---|---|---|
| best C++ run | 9600.77 | 898 | C++, earlier revision (Nov 2026); settings not recorded |
| bounded C++ run | 9473.45 | 888 | current C++, 25 s, 2 threads, seed 42, loaded 4-core 2015 laptop |

The assignment brief quotes an instructor reference solution of EUR 9,060. That number comes from
the brief, which is not included, and was not re-run.

**Not measured:** multi-seed statistics, thread scaling, and idle-machine throughput.

## 9. Limitations
- **The C++ solver is not reproducible:**
  - its temperature follows wall-clock time, and threads exchange solutions at timing-dependent
    moments, so the same seed gives different results. Three 25 s runs with seed 42 before the
    Phase 2 fixes gave 9459.47 and 9448.43 (both validated) and 9452.83 (solver-reported only);
  - the saved record files do not record their settings, and were produced by earlier source
    revisions.
- **Fixed problem shape:** 20 consultants and depot node 251 are compile-time constants.
- **`dead`-order pruning:** orders whose profit does not cover their service time are never
  inserted. This is exact only under the triangle inequality. The course matrix violates it by at
  most 2 s, so at most 2/60 EUR per such order can be missed.
- **Sanity checks:** `selfCheck()` runs at the end of each thread and on warm start, not
  periodically. The solver does not write intermediate best solutions.
- **No exact model:** a MIP formulation from the original project was never solved and is not
  included.

## 10. Reproducibility
- **Data:** the course instance is not redistributable and is not included.
  `tools/make_instance.py --seed N` generates deterministic synthetic instances.
- **Seed:** pass a seed as the third argument. The random streams are then fixed, but results still
  depend on timing.
- **Can be regenerated:** synthetic instances, all tests, and the validation of any solution file
  against an instance you have.
- **Cannot be regenerated here:** the course-instance scores, without that instance. The exact runs
  behind the saved record files cannot be regenerated by anyone.

## 11. Context, attribution & licence
- **Course:** this work started as the final programming assignment of *Modelling and Computing*
  (BSc Business Analytics, Maastricht University, March 2025).
- **Team:** the graded March 2025 version was built by a **team of three** (names withheld pending
  consent). It is not in this repository.
- **Author's solo work:** `cpp/sa.cpp` is the author's solo rewrite from November 2026. It is not
  a port of the team code.
- **AI assistance:**
  - **2025 team submission:** developed with the help of generative AI; this use was acknowledged in the
    submitted course report, as the course required.
  - **2026 C++ rebuild:** also developed with AI assistance. The specific tools used and the extent of
    their contribution were not recorded and are not specified here.
  - **2026 portfolio preparation:** the input-validation fixes, regression tests, the shared solution
    validator and the documentation were done with Claude Code under
    my direction. I reviewed the changes.
  - All reported solutions were verified feasible by the included validator; details are in
    [docs/PROVENANCE.md](docs/PROVENANCE.md).
- **Data and checker:** the course instance and the course's solution checker are not included.
  `tools/validate_solution.py` was written independently from the problem definition.
- **Provenance and every change:** [`docs/PROVENANCE.md`](docs/PROVENANCE.md).
- **License:** MIT — see [`LICENSE`](LICENSE).
