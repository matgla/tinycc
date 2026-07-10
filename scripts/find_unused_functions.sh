#!/bin/sh
# Whole-program unused-function scan; see docs/find_unused_functions.md
set -e
cd "$(dirname "$0")/.."

WITH_TESTS=1
for arg in "$@"; do
  case "$arg" in
    --no-tests) WITH_TESTS=0 ;;
    -h|--help)
      echo "usage: $0 [--no-tests]"
      echo "  --no-tests  don't count tests/unit sources as callers"
      exit 0 ;;
  esac
done

SOURCES="$(ls ./*.c) $(find ir arch -name '*.c')"
if [ "$WITH_TESTS" = 1 ]; then
  SOURCES="$SOURCES $(find tests/unit -name '*.c' 2>/dev/null)"
fi

# Pin the armv8m build config (see Makefile DEF-armv8m); TCC_LOG_ALL and
# CONFIG_TCC_DEBUG keep debug-only call sites visible.
cppcheck --enable=unusedFunction --quiet \
  -DTCC_TARGET_ARM -DTCC_ARM_VFP -DTCC_ARM_EABI -DTCC_ARM_HARDFLOAT \
  -DTCC_TARGET_ARM_THUMB -DTCC_TARGET_ARM_ARCHV8M \
  -DTCC_LOG_ALL=1 -DCONFIG_TCC_DEBUG=1 \
  -I. -Iir -Iir/opt -Iarch -Iarch/arm -Iarch/arm/thumb \
  $SOURCES 2>&1 \
  | grep '\[unusedFunction\]' \
  | grep -v '^tests/' \
  | sort -u
