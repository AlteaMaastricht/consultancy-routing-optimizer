#!/bin/sh
# Demo on a synthetic instance: generate data, run the C++ solver and validate the solution.
# Run from the repository root after building:  sh examples/run_demo.sh
set -e
[ -x build/sa ] || { echo "build first: cmake -S . -B build && cmake --build build"; exit 1; }
mkdir -p data/demo
python3 tools/make_instance.py --out-dir data/demo --orders 400 --nodes 400 --seed 7

# C++ solver: 10 s on 2 threads, seed 42 (writes output.txt in its working directory)
(cd data/demo && SA_THREADS=2 ../../build/sa 10 "" 42 > run_cpp.log)
python3 tools/validate_solution.py --orders data/demo/orders.txt --times data/demo/drivingtimes.txt \
    data/demo/output.txt
