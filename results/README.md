# Results on the course-provided instance (instance and solution files not distributed)

The solution files (order IDs per consultant, in visiting order) are **not included in this
repository**. They refer to the course-provided instance (1,177 orders, 1,099 nodes), which is not
distributed either, and permission to publish derived files has not been confirmed. The table records
what was verified. Someone with the instance and a solution file can re-check it with:
```sh
python3 tools/validate_solution.py --orders <course>/orders.txt --times <course>/drivingtimes.txt <solution file>
```
All files were checked on 17–18 Nov 2026 with `tools/validate_solution.py` (an independent
validator) and cross-checked with the course's own checker. All are feasible, and each first line
equals the recomputed net profit rounded down. These are heuristic results; no optimum or bound is
known.

| Run (file not distributed) | Net profit (EUR) | Orders | Produced by | Notes |
|---|---|---|---|---|
| `solution_9600.txt` | 9600.77 | 898 | C++ solver (`cpp/sa.cpp`), earlier revision, 16 Nov 2026 | settings not recorded |
| `solution_9528.txt` | 9528.82 | 894 | C++ solver, earlier revision, 17 Nov 2026 | settings not recorded |
| `bounded_cpp_phase2_25s_seed42_2threads.txt` (+ `.log`) | 9473.45 | 888 | current `cpp/sa.cpp`, `SA_THREADS=2 sa 25 "" 42`, 18 Nov 2026 | 71M iterations; single run, not deterministic |
| `bounded_run_25s_seed42_2threads.txt` (+ `.log`) | 9448.43 | 883 | `cpp/sa.cpp` before the Phase 2 input fixes (same algorithm), 17 Nov 2026 | 51M iterations |

**Machine for the re-runs:** MacBook Air, Intel i5-5350U (2 cores / 4 threads), 8 GB, macOS 12.7.6.
The machine was shared with other jobs (load average 12–16), so the throughput numbers in the logs
are not idle-machine measurements. The solver's schedule follows wall-clock time, so the same
seed does not reproduce the same result.
