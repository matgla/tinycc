#!/bin/bash
# Run armv8m-tcc under valgrind for each IR test source file.
# Usage: ./scripts/valgrind_compile_tests.sh [pattern]
#   pattern: optional glob to filter test files (e.g. "pr68*" or "20_*")
#
# Reports any test with valgrind errors.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TCC_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TCC="$TCC_ROOT/armv8m-tcc"
COMMON_FLAGS="-nostdlib -fvisibility=hidden -mcpu=cortex-m33 -mthumb -mfloat-abi=soft -ffunction-sections -O0"
INCLUDE_FLAGS="-I $TCC_ROOT/tests/ir_tests/libc_includes -I $TCC_ROOT/tests/ir_tests/libc_imports -I $TCC_ROOT/tests/ir_tests/libc_includes/newlib -I /usr/arm-none-eabi/include -I $TCC_ROOT/include"
OUTDIR=$(mktemp -d)
PATTERN="${1:-*}"
ERRORS=0
TOTAL=0
FAILED_FILES=""

# Collect test files
shopt -s nullglob
IR_TESTS=($TCC_ROOT/tests/ir_tests/${PATTERN}.c)
GCC_TESTS=($TCC_ROOT/tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/${PATTERN}.c)
shopt -u nullglob

ALL_TESTS=("${IR_TESTS[@]}" "${GCC_TESTS[@]}")

echo "Running valgrind on ${#ALL_TESTS[@]} test files..."
echo "Output dir: $OUTDIR"
echo ""

for src in "${ALL_TESTS[@]}"; do
    [ -f "$src" ] || continue
    name=$(basename "$src" .c)
    TOTAL=$((TOTAL + 1))

    out="$OUTDIR/${name}.o"
    vg_log="$OUTDIR/${name}.valgrind"

    valgrind --error-exitcode=99 --errors-for-leak-kinds=none --leak-check=no \
        --track-origins=yes -q \
        $TCC $COMMON_FLAGS $INCLUDE_FLAGS -c "$src" -o "$out" \
        2>"$vg_log"
    rc=$?

    if [ $rc -eq 99 ]; then
        ERRORS=$((ERRORS + 1))
        FAILED_FILES="$FAILED_FILES $name"
        echo "FAIL: $name"
        head -20 "$vg_log"
        echo "---"
    elif [ $rc -ne 0 ]; then
        # Compile error (not valgrind) - skip silently
        :
    else
        # Clean
        rm -f "$out" "$vg_log"
    fi

    # Progress every 100 tests
    if [ $((TOTAL % 100)) -eq 0 ]; then
        echo "  ... $TOTAL tests checked ($ERRORS errors so far)"
    fi
done

echo ""
echo "=============================="
echo "Total: $TOTAL  Valgrind errors: $ERRORS"
if [ $ERRORS -gt 0 ]; then
    echo "Failed tests:$FAILED_FILES"
    echo "Valgrind logs in: $OUTDIR"
    exit 1
else
    echo "All clean!"
    rm -rf "$OUTDIR"
    exit 0
fi
