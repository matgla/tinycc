#!/usr/bin/env bash
#
# asan_sweep.sh — Phase BH / Track 1 ASAN+UBSan corpus sweep for tinycc.
#
# The cross compiler armv8m-tcc is built with AddressSanitizer ON by default
# (config.mak: -fsanitize=address), so compiling any corpus file *with* it makes
# tcc report ASAN/LeakSanitizer errors on its OWN heap bugs.  The ORACLE is the
# sanitizer output printed by tcc, not the compile exit code: a plain
# "unsupported feature" compile error is NOT a hit.
#
# This sweeps the corpus (gcc-torture compile+execute, tests2, ir_tests) across
# -O0/-O1/-O2, greps stderr for sanitizer signatures, and dedups hits by the top
# meaningful backtrace frames so one bug across many files collapses to one entry.
#
# Test/tooling only.  Does NOT modify production code.  --with-ubsan builds a
# SEPARATE compiler out-of-band (config.mak is saved+restored) so the shared
# armv8m-tcc other agents depend on is never mutated.
#
# Usage:
#   scripts/asan_sweep.sh [options]
#
#   --corpus C        gcc-torture | tests2 | ir_tests | all   (default: all)
#   --olevels L       comma list of opt levels   (default: -O0,-O1,-O2)
#   --shard i/N       sweep only shard i of N (1-based) for parallel runs
#   --limit N         cap number of files swept (after sharding)
#   --timeout S       per-compile timeout in seconds (default: 60)
#   --compiler PATH   compiler to use (default: ./armv8m-tcc; the ASAN build)
#   --with-ubsan      ALSO build an out-of-band UBSan compiler and sweep with it
#                     (rebuilds into a temp dir, restoring config.mak; SLOW)
#   --report PATH     write the deduped report to PATH (also printed)
#   --raw-hits PATH   append every raw hit line (file|olevel|key) to PATH
#   -h | --help       show this help
#
# Examples:
#   # full sweep, all corpora, all O-levels:
#   scripts/asan_sweep.sh --corpus all
#   # one shard of gcc-torture for a parallel fleet:
#   scripts/asan_sweep.sh --corpus gcc-torture --shard 3/40
#   # quick smoke:
#   scripts/asan_sweep.sh --corpus tests2 --limit 30
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
HELPER="$SCRIPT_DIR/asan_sweep.py"

# ---- defaults ----
CORPUS="all"
OLEVELS="-O0,-O1,-O2"
SHARD=""
LIMIT="0"
TIMEOUT="60"
COMPILER="$REPO/armv8m-tcc"
WITH_UBSAN="0"
REPORT=""
RAW_HITS=""

usage() { sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --corpus)   CORPUS="$2"; shift 2;;
    --olevels)  OLEVELS="$2"; shift 2;;
    --shard)    SHARD="$2"; shift 2;;
    --limit)    LIMIT="$2"; shift 2;;
    --timeout)  TIMEOUT="$2"; shift 2;;
    --compiler) COMPILER="$2"; shift 2;;
    --with-ubsan) WITH_UBSAN="1"; shift;;
    --report)   REPORT="$2"; shift 2;;
    --raw-hits) RAW_HITS="$2"; shift 2;;
    -h|--help)  usage; exit 0;;
    *) echo "unknown option: $1" >&2; usage; exit 2;;
  esac
done

# --------------------------------------------------------------------------
# Reconstruct the EXACT include/ABI flags the real torture harness passes when
# CC is armv8m-tcc.  Mirrors tests/ir_tests/qemu/mps2-an505/Makefile:
#   GCC_ABI_FLAGS = -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
#   CFLAGS += -nostdlib -fvisibility=hidden $(GCC_ABI_FLAGS) -ffunction-sections
#   (armv8m-tcc branch) -I libc_includes -I libc_imports -I newlib
#                       -I $(ARM_SYSROOT)/include -I $(TCC_PATH)/include
# --------------------------------------------------------------------------
GCC_ABI_FLAGS="-mcpu=cortex-m33 -mthumb -mfloat-abi=soft"
ABI_FLAGS="-nostdlib -fvisibility=hidden $GCC_ABI_FLAGS -ffunction-sections"

LIBC_INCLUDES="$(realpath "$REPO/tests/ir_tests/libc_includes")"
LIBC_IMPORTS="$(realpath "$REPO/tests/ir_tests/libc_imports")"
NEWLIB_INCLUDES="$LIBC_INCLUDES/newlib"
ARM_SYSROOT="$(arm-none-eabi-gcc $GCC_ABI_FLAGS --print-sysroot 2>/dev/null || echo /usr/arm-none-eabi)"
INCLUDE_FLAGS="-I$LIBC_INCLUDES -I$LIBC_IMPORTS -I$NEWLIB_INCLUDES -I$ARM_SYSROOT/include -I$REPO/include"

run_sweep() {
  local compiler="$1" tag="$2" report_arg=()
  echo "================================================================"
  echo " Sweep ($tag): $compiler"
  echo "================================================================"
  local report_path=""
  if [[ -n "$REPORT" ]]; then
    if [[ "$tag" == "ubsan" ]]; then
      report_path="${REPORT%.txt}.ubsan.txt"
    else
      report_path="$REPORT"
    fi
    report_arg=(--report "$report_path")
  fi
  local raw_arg=()
  [[ -n "$RAW_HITS" ]] && raw_arg=(--list-hits-raw "$RAW_HITS")
  local shard_arg=()
  [[ -n "$SHARD" ]] && shard_arg=(--shard "$SHARD")

  # Values that begin with '-' (olevels, the -I/-m flag bundles) are passed with
  # '=' so argparse does not mistake them for options.
  python3 "$HELPER" \
    --compiler "$compiler" \
    --corpus "$CORPUS" \
    --olevels="$OLEVELS" \
    --limit "$LIMIT" \
    --timeout "$TIMEOUT" \
    --include-flags="$INCLUDE_FLAGS" \
    --abi-flags="$ABI_FLAGS" \
    "${shard_arg[@]}" \
    "${report_arg[@]}" \
    "${raw_arg[@]}"
}

# ---- ASAN sweep (the default, using the existing shared compiler) ----
if [[ ! -x "$COMPILER" ]]; then
  echo "error: compiler not found or not executable: $COMPILER" >&2
  echo "       build it with 'make cross' first." >&2
  exit 2
fi
run_sweep "$COMPILER" "asan"

# ---- optional out-of-band UBSan sweep ----
if [[ "$WITH_UBSAN" == "1" ]]; then
  echo
  echo "################################################################"
  echo "# --with-ubsan: building a SEPARATE UBSan compiler out-of-band"
  echo "# (config.mak is saved + restored; shared armv8m-tcc untouched)"
  echo "################################################################"

  UBSAN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/asan_sweep_ubsan.XXXXXX")"
  CONFIG_BAK="$(mktemp "${TMPDIR:-/tmp}/config.mak.bak.XXXXXX")"
  cp "$REPO/config.mak" "$CONFIG_BAK"

  restore_config() {
    cp "$CONFIG_BAK" "$REPO/config.mak"
    rm -f "$CONFIG_BAK"
    echo "restored config.mak"
  }
  trap restore_config EXIT

  UBSAN_TCC="$UBSAN_DIR/armv8m-tcc"
  (
    cd "$REPO"
    # Reconfigure with UBSan (this rewrites config.mak — restored on exit).
    ./configure --enable-ubsan >/dev/null
    # Build the cross compiler into the temp dir without clobbering the shared
    # armv8m-tcc: build normally, then move the artifact aside and restore the
    # shared one from git (it is a tracked binary in this repo layout — if not,
    # the ASAN compiler is rebuilt by the next 'make cross' anyway).
    make cross >/dev/null 2>&1 || { echo "UBSan build failed" >&2; exit 1; }
    cp "$REPO/armv8m-tcc" "$UBSAN_TCC"
  )
  # Rebuild the shared ASAN compiler so concurrent agents see it unchanged.
  restore_config
  trap - EXIT
  ( cd "$REPO" && make cross >/dev/null 2>&1 ) || \
    echo "warning: could not rebuild shared ASAN armv8m-tcc; run 'make cross'" >&2

  run_sweep "$UBSAN_TCC" "ubsan"
  rm -rf "$UBSAN_DIR"
fi
