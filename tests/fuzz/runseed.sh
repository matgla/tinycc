#!/usr/bin/env bash
# runseed.sh — compile one C file with the armv8m-tcc cross compiler against the
# mps2-an505 newlib and run it under QEMU, printing the program's result line.
#
#   runseed.sh <src.c> <-Ox> [extra tcc flags...]
#
# Prints exactly one token:  checksum=<hex> | HardFault | Lockup | COMPILE_FAIL
#
# Toolchain paths are derived (no hard-coded gcc version) so it survives
# arm-none-eabi-gcc upgrades.  Requires: armv8m-tcc built (`make cross`),
# arm-none-eabi-gcc, qemu-system-arm, and the mps2 newlib_build present
# (the IR test-suite builds it on first run).
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"   # libs/tinycc
TCC="$ROOT/armv8m-tcc"
MPS="$ROOT/tests/ir_tests/qemu/mps2-an505"
NL="$MPS/newlib_build"
INC="-I$ROOT/tests/ir_tests/libc_includes -I$ROOT/tests/ir_tests/libc_imports -I$ROOT/tests/ir_tests/libc_includes/newlib -I/include -I$ROOT/include"
ARMCC=(arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfloat-abi=soft)

[ -x "$TCC" ] || { echo "NO_TCC (run 'make cross' in $ROOT)"; exit 2; }

# Toolchain objects for the correct (thumb/v8-m.main/nofp) multilib.
CRTI="$("${ARMCC[@]}" -print-file-name=crti.o)"
CRTN="$("${ARMCC[@]}" -print-file-name=crtn.o)"
CRTEND="$("${ARMCC[@]}" -print-file-name=crtend.o)"
LIBGCC="$("${ARMCC[@]}" -print-libgcc-file-name)"
RDIMON_CRT0="$(find "$NL" -name rdimon-crt0.o 2>/dev/null | head -1)"
LIBRDIMON="$(find "$NL" -name librdimon.a 2>/dev/null | head -1)"
LIBC="$(find "$NL" -path '*newlib*' -name libc.a 2>/dev/null | head -1)"
LIBM="$(find "$NL" -path '*newlib*' -name libm.a 2>/dev/null | head -1)"

# One compile+run attempt.  Emits: checksum=<hex> | HardFault | Lockup |
# COMPILE_FAIL (tcc itself errored — deterministic) | INFRA_FAIL (no object but
# tcc printed no error — transient: full /tmp, killed child, fd/PID exhaustion).
_runseed_once() {
  local src="$1" opt="$2"; shift 2
  local cf="-nostdlib -fvisibility=hidden -mcpu=cortex-m33 -mthumb -mfloat-abi=soft -ffunction-sections $opt $* $INC"
  local d; d="$(mktemp -d 2>/dev/null)"
  [ -n "$d" ] && [ -d "$d" ] || { echo "INFRA_FAIL"; return; }
  local err="$d/err.log"
  # NB: tcc prints "Memory region ..." to stdout during the link — suppress
  # stdout; keep stderr (to tell a real tcc error from a transient infra fail).
  "$TCC" $cf -c "$MPS/boot.S" -o "$d/boot.o" >/dev/null 2>"$err"
  "$TCC" $cf "$src" "$d/boot.o" "$CRTI" "$RDIMON_CRT0" "$CRTEND" "$CRTN" \
    -o "$d/m.elf" -Wl,--gc-sections -B"$ROOT" -L"$ROOT/lib" -L"$ROOT/lib/fp" -L"$ROOT" \
    -Wl,--start-group -larmv8m-libtcc1.a -lsoftfp "$LIBC" "$LIBRDIMON" "$LIBM" "$LIBGCC" \
    -Wl,--end-group -Wl,-oformat=elf32-littlearm -T"$MPS/linker_script.ld" >/dev/null 2>>"$err"
  if [ ! -f "$d/m.elf" ]; then
    if grep -qiE "error:|compiler_error|assert|signal|Sanitizer" "$err"; then echo "COMPILE_FAIL"
    else echo "INFRA_FAIL"; fi
    rm -rf "$d"; return
  fi
  timeout 20 qemu-system-arm -machine mps2-an505 -nographic -semihosting -kernel "$d/m.elf" 2>&1 \
    | grep -oE "checksum=[0-9a-f]+|HardFault|Lockup" | head -1
  rm -rf "$d"
}

# Self-healing wrapper: retry transient INFRA_FAILs (up to 2x) so a loaded host
# can't masquerade a real result as a compile failure.  External contract stays
# checksum=<hex> | HardFault | Lockup | COMPILE_FAIL.
runseed() {
  local r i
  for i in 1 2 3; do
    r="$(_runseed_once "$@")"
    [ "$r" = INFRA_FAIL ] || { echo "$r"; return; }
  done
  echo "COMPILE_FAIL"   # persistent infra failure — surface it, don't hide it
}

# Export everything a parallel (xargs / GNU parallel) subshell needs so callers
# can fan out runseed without re-deriving paths per worker.
export ROOT TCC MPS NL INC CRTI CRTN CRTEND LIBGCC RDIMON_CRT0 LIBRDIMON LIBC LIBM
export -f runseed _runseed_once

# Allow standalone use:  runseed.sh foo.c -O2 [-fno-...]
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  [ $# -ge 2 ] || { echo "usage: runseed.sh <src.c> <-Ox> [tcc flags...]"; exit 2; }
  out="$(runseed "$@")"; echo "${out:-NO_OUTPUT}"
fi
