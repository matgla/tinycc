#!/usr/bin/env bash
#
# coverage_tccgen.sh — merged line-coverage report for tccgen.c.
#
# tccgen.c is the C parser / type checker / IR-emission frontend.  Most of it
# only runs inside the full compile pipeline, so the isolated unit-test binary
# (tests/unit/arm/armv8m, which #includes tccgen.c and stubs the IR/ELF/pp
# boundary) can only reach its pure `static` helpers.  This script produces the
# *merged* picture by combining two coverage sources for the same source file:
#
#   1. the real cross compiler (armv8m-tcc) with tccgen.c instrumented, run over
#      the whole compile-test corpus (ir_tests, tests2, frontend, gcc-torture) at
#      several -O levels — this exercises the parser/codegen pipeline; and
#   2. the isolated tccgen unit tests (make -C tests/unit/arm/armv8m COVERAGE=1),
#      which cover the leaf helpers and error/diagnostic branches the -O2 build
#      folds away.
#
# The two are unioned per source line with lcov (gcovr's line-keyed merge cannot
# combine two different compilations of the same file — the -O0 unit build and
# the -O2 real build expose different executable-line sets).
#
# Only tccgen.o is instrumented (not the whole compiler): a full-tree --coverage
# build makes every armv8m-tcc invocation flush ~80 .gcda files, ~10x slower.
#
# The normal (uninstrumented) build is restored on exit — armv8m-tcc is left
# exactly as it was found, so this is safe to run against a working tree.
#
# Env knobs:
#   COV_TARGET   cross target prefix (default: armv8m)
#   COV_JOBS     parallel compiles (default: 8)
#   COV_OLEVELS  optimisation levels to sweep (default: "-O0 -O2")
#   COV_OUT      output directory (default: <top>/coverage-tccgen)
#   COV_NO_TORTURE=1  skip the gcc-torture corpus (faster)
#
# Usage:  make coverage-tccgen        (preferred)
#         scripts/coverage_tccgen.sh
#
set -euo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$TOP"

TARGET="${COV_TARGET:-armv8m}"
X="${TARGET}-"
JOBS="${COV_JOBS:-8}"
OLEVELS="${COV_OLEVELS:--O0 -O1 -O2 -Os}"
OUT="${COV_OUT:-$TOP/coverage-tccgen}"
UNITDIR="tests/unit/arm/armv8m"
UNIT_GCDA="$UNITDIR/build_tccgen/test_tccgen.gcda"
OBJ="${X}tccgen.o"
BIN="${X}tcc"
GCNO="${X}tccgen.gcno"
GCDA="${X}tccgen.gcda"

for t in lcov geninfo genhtml; do
  command -v "$t" >/dev/null || { echo "coverage_tccgen: missing required tool '$t'" >&2; exit 2; }
done

WORK="$(mktemp -d)"
cleanup() {
  echo "==> restoring normal (uninstrumented) build"
  rm -f "$OBJ" "$BIN" "$GCNO" "$GCDA"
  make cross >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

echo "==> building baseline cross compiler"
make cross >/dev/null

echo "==> instrumenting $(basename "$OBJ") (tccgen.c only) and relinking $BIN"
rm -f "$OBJ" "$BIN"
CC_CMD="$(make -n CROSS_TARGET="$TARGET" "$OBJ" | grep -E "(gcc|cc).* -c tccgen\.c( |$)" | head -1)"
[ -n "$CC_CMD" ] || { echo "coverage_tccgen: could not derive compile command for $OBJ" >&2; exit 3; }
eval "$CC_CMD --coverage -fprofile-update=atomic"
LINK_CMD="$(make -n CROSS_TARGET="$TARGET" "$BIN" | grep -E "(gcc|cc) -o ${BIN} " | head -1)"
[ -n "$LINK_CMD" ] || { echo "coverage_tccgen: could not derive link command for $BIN" >&2; exit 3; }
eval "$LINK_CMD --coverage"

echo "==> unit-test coverage ($UNITDIR)"
make -C "$UNITDIR" COVERAGE=1 run-tccgen >/dev/null

echo "==> compiling test corpus through instrumented $BIN (levels:$OLEVELS jobs:$JOBS)"
LIBC="$TOP/tests/ir_tests/libc_includes"
INC="-I$LIBC -I$TOP/tests/ir_tests/libc_imports -I$LIBC/newlib -I$TOP/include"
ARMF="-mcpu=cortex-m33 -mthumb -mfloat-abi=soft"

# Corpus lists (only what exists).  ir_tests/tests2/frontend want the newlib
# include set + ARM flags; gcc-torture is freestanding.
: > "$WORK/inc.list"
: > "$WORK/free.list"
ls "$TOP"/tests/ir_tests/*.c            2>/dev/null >> "$WORK/inc.list" || true
ls "$TOP"/tests/tests2/*.c              2>/dev/null >> "$WORK/inc.list" || true
find "$TOP/tests/frontend" -name '*.c'  2>/dev/null >> "$WORK/inc.list" || true
if [ "${COV_NO_TORTURE:-0}" != "1" ]; then
  find "$TOP/tests/gcctestsuite" -name '*.c' 2>/dev/null >> "$WORK/free.list" || true
fi

sweep() { # $1=list  $2...=extra flags
  local list="$1"; shift
  [ -s "$list" ] || return 0
  local O
  for O in $OLEVELS; do
    xargs -a "$list" -P "$JOBS" -I{} \
      sh -c '"$1" -c -w $2 "$3" -o /dev/null >/dev/null 2>&1 || true' \
      sh "$TOP/$BIN" "$*" {}
  done
}
# shellcheck disable=SC2086
sweep "$WORK/inc.list" $ARMF $INC
# shellcheck disable=SC2086
sweep "$WORK/free.list"

[ -f "$GCDA" ] || { echo "coverage_tccgen: no $GCDA produced — corpus empty?" >&2; exit 4; }

echo "==> merging coverage (real corpus + unit tests)"
geninfo "$GCDA"       -o "$WORK/real.info" --gcov-tool gcov -q 2>/dev/null
geninfo "$UNIT_GCDA"  -o "$WORK/unit.info" --gcov-tool gcov -q 2>/dev/null
lcov -a "$WORK/real.info" -a "$WORK/unit.info" -o "$WORK/merged.info" \
     --rc geninfo_unexecuted_blocks=1 -q 2>/dev/null
lcov --extract "$WORK/merged.info" '*/tccgen.c' -o "$WORK/tccgen.info" -q 2>/dev/null

mkdir -p "$OUT"
cp "$WORK/tccgen.info" "$OUT/tccgen.info"
genhtml "$WORK/tccgen.info" -o "$OUT" -q \
  --title "tccgen.c merged coverage (unit tests + real-compiler corpus)" 2>/dev/null

echo ""
echo "======================= tccgen.c merged coverage ======================="
lcov --summary "$WORK/tccgen.info" 2>/dev/null | grep -iE 'lines|functions' || true
echo "  report:    $OUT/index.html"
echo "  tracefile: $OUT/tccgen.info"
echo "========================================================================"
