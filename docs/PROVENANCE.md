# Provenance

This repository consolidates two staging copies prepared in November 2026 (decision D2, option
"merge"):
- **04** `consultancy-routing-optimizer`: Python solvers from February 2026 (the author's solo
  continuation of the March 2025 team assignment). Original folder: `Final 7000/`.
- **08** `simulated-annealing-c-project`: the author's solo C++17 rewrite from November 2026.
  Original folder: `2026 Project  rebuild in C/`.

The reasoning behind the merge is in the audit workspace (`consolidation_plan.md`, not part of the
repository).

## Origin and authorship
- **Course:** *Modelling and Computing*, BSc Business Analytics, Maastricht University, final
  programming assignment, March 2025.
  - The graded March 2025 submission was a **team of three**. Names are withheld pending consent
    (D3), and the author's individual share is not documented.
  - That submission is **not** included.
- **`python/`:** the author's solo continuation, with file dates 21–24 Feb 2026. It builds on the
  team-era code lineage.
- **`cpp/sa.cpp`:** the author's solo rewrite, written 16–17 Nov 2026. It is not a port of the team
  code.
- **AI assistance (author-confirmed):** the 2025 team submission used generative AI, acknowledged in the
  submitted course report; the 2026 C++ rebuild used AI assistance, with tools and extent not recorded.
  Neither codebase marks individual AI-generated sections.

## Every file → origin and changes
| File | Origin | Changes |
|---|---|---|
| `cpp/sa.cpp` | 08: `sa.cpp` (17 Nov 2026 00:41) | Phase 1: corrected the header default budget (80 s, not 300) and added `SA_DATA_DIR`. Phase 2: input-handling fixes (table below). The algorithm is unchanged. |
| `python/sa_final.py` | 04: `SA_final.py` (22 Feb 2026) | renamed only (byte-identical) |
| `python/sa_optimised.py` | 04: `SA algorithm.py` (22 Feb 2026) | renamed only (byte-identical) |
| `tools/validate_solution.py` | written for 04/08 staging (Nov 2026) | identical copy |
| `tools/make_instance.py` | written for 04/08 staging | identical copy |
| `tools/run_python_sa.py` | written for 04 staging | solver directory changed from `src/` to `python/` |
| `tests/test_validator.py` | written for 04/08 staging | identical copy |
| `tests/test_python_solvers.py` | 04 staging `tests/test_solvers.py` | removed the C++ port test (port not included); added a seeded-determinism test |
| `tests/test_cpp_solver.py` | 08 staging `tests/test_solver.py` | renamed |
| `tests/test_cpp_input_validation.py` | 08 staging, Phase 2 regression tests | renamed |
| `CMakeLists.txt` | 08 staging | source path `cpp/sa.cpp`; project name |
| `requirements.txt` | 04 staging | identical |
| `.gitignore` | 08 staging + Python entries | merged |
| `examples/run_demo.sh` | new | runs both solvers on a synthetic instance |
| `results/course_instance/solution_9600.txt`, `solution_9528.txt` | 08: `output RECORD 9600.txt`, `output.txt` | renamed |
| `results/course_instance/bounded_run_25s_seed42_2threads.{txt,log}` | 08 staging re-run (17 Nov 2026, before Phase 2) | — |
| `results/course_instance/bounded_cpp_phase2_25s_seed42_2threads.{txt,log}` | new re-run with the current `cpp/sa.cpp` (18 Nov 2026) | — |
| `results/course_instance/solution_{9485,9324,9202,9191}.txt` | 04: `output RECORD 9485.txt`, `output 9324 Record.txt`, `output RECORD 9200.txt`, `output.txt` | renamed |
| `results/course_instance/bounded_python_final_1M_iter_seed1.txt` | 04 staging re-run (17 Nov 2026) | — |
| `README.md`, `results/README.md`, `docs/ARCHITECTURE.md`, `docs/PROVENANCE.md` | new (ARCHITECTURE merged from both staging copies) | — |

## Phase 2 input-handling fixes in `cpp/sa.cpp` (18 Nov 2026)
Each fix was made test-first: the regression tests in `tests/test_cpp_input_validation.py` failed
on the unfixed code (15 of 16) and pass after the fix.

| # | Original behaviour | Fix |
|---|---|---|
| 1 | A warm-start route length above 120 wrote past fixed arrays (ASan `global-buffer-overflow`, or SIGABRT). Truncated or non-numeric files were rejected silently or misread. | Strict parsing; route length must be in `0..MAXL-3`; every rejection is reported, and the run continues without the warm start. |
| 2 | `std::stoi` aborted on `XX` or empty fields and silently truncated `12.5`; matrix rows were cut at the first bad token; eligibility flag `2` was accepted. | `strtoll`-based parsing with full consumption and range checks. Errors are reported as `file:line: message`, exit 1. Also enforced: a square matrix, flags in {0, 1}, at least 27 columns, no duplicate order IDs. |
| 3 | A matrix without node 251 silently used zero depot distances (reported 849.60, true 78.52). An order node outside the matrix caused SIGSEGV. | Clear error and exit 1. The depot is part of the problem definition, so substituting another node silently would hide bad data. |
| 4 | A 1-order instance hit integer division by zero (`q % 0`). Zero orders "ran" and exited 0. | The order is used as its own sentinel neighbour, so a 1-order instance now runs. Zero orders gives an error. |
| + | More than 1,279 orders or 1,200 nodes overran arrays; times above 65,535 s wrapped in `uint16`. | Capacity checks; driving times must be in 0..65,535. |

**Unchanged on valid input:**
- the saved course solutions load with identical evaluations (9600.77, 9528.82, 9448.43) before
  and after the fix;
- the rewritten output files are byte-identical to the inputs;
- for N ≥ 2, the neighbour lists consume the RNG exactly as before.

## Not included (with reason)
| Item | Reason |
|---|---|
| 04's C++ port (`Final 7000/sa.cpp`) | redundant: a line-by-line port of `sa_optimised.py`. It is not seedable, has the instance-tuned `best_fitness > 9350.0` reheat trigger (line 869) and no input checks, and was never run on the course instance. It remains in the 04 staging copy. |
| `orders.txt`, `drivingtimes.txt` | course-provided data, not redistributable |
| `Checker.py` (both folders) | course-provided checker (instructor code). The November copy differs from the original only in its hard-coded solution filename. |
| `gurobi.py` | MIP formulation that was never solved (no output; it needs a full licence) |
| Binaries (`sa`, `sa_new`, `sa_dbg`), duplicates (`sa copy.cpp`, `8.py`, `mainDS copy*.py`, `operators.py`), `output8000.txt`, editor and OS files, team-era and other-language folders | build output, duplicates, unknown provenance, or obsolete code (see the 04 audit) |

## Phase 4 changes (18 Nov 2026)
- `LICENSE` added: MIT, copyright 2026 Arthur J. Maarschalkerweerd (author decision D6); README licence line updated.
- `CMakeLists.txt`: new opt-in `SA_SANITIZE` option (ASan + UBSan), so sanitizer builds no longer need manual compiler flags; documented in README §6. Default build unchanged.

## D14b (18 Nov 2026)
- The 11 course-instance result files (9 solutions + 2 logs) were moved out of the publication copy (kept in `_PORTFOLIO_AUDIT/EIGHT_REPOSITORIES/excluded/consultancy-routing-optimizer-merged/`). They derive from the course instance, and permission to distribute them is unconfirmed. README and `results/README.md` keep the verified scores, labelled "file not distributed". Rows 43–47 above describe where those files came from.

- README §11: AI-assistance statement in its final wording (decision D7); `Status: preparation draft` line removed for publication.

## C++-only scope (19 Nov 2026, author decision)
The repository was reduced to the C++ implementation. Removed: `python/sa_final.py`,
`python/sa_optimised.py`, `tools/run_python_sa.py`, `tests/test_python_solvers.py` and
`requirements.txt` (only the Python solvers needed numpy/matplotlib). Results produced by Python or
February 2026 code were dropped from the README and `results/README.md`. The validator, the instance
generator and the C++ tests remain (Python standard library only). The repository history was
then squashed into a single commit, so the removed files are no longer in it. The rows above that
mention `python/`, `run_python_sa.py` or the February 2026 result files describe the earlier
staging contents.

- `.gitattributes` added (19 Nov 2026): `tests/` and `tools/` are marked `linguist-vendored` so GitHub lists the repository as C++. The files are unchanged and still in the repository.
