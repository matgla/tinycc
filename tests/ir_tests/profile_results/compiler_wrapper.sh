#!/bin/bash
# Profile wrapper for TinyCC compiler
METRICS_FILE="/home/mateusz/repos/tinycc/tests/ir_tests/profile_results/metrics_01_hello_world.txt"
REAL_COMPILER="/home/mateusz/repos/tinycc/armv8m-tcc"

# Run compiler with GNU time, append metrics
/usr/bin/time -v -a -o "$METRICS_FILE" "$REAL_COMPILER" "$@"
EXIT_CODE=$?

exit $EXIT_CODE
