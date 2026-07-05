#!/usr/bin/env bash
# triage_olevels.sh — sweep a seed range for O-level miscompiles and triage each.
#
#   tests/fuzz/triage_olevels.sh [LO] [HI] [JOBS]      # default 0 4999 16
#   SEEDS="588 860 1005" tests/fuzz/triage_olevels.sh  # triage an explicit list
#
# For every failing seed it records, in a markdown report:
#   - the gcc -m32 -funsigned-char ground truth (ARM ABI: unsigned char, 32-bit long)
#   - tcc output at O0/O1/O2/Os and which level(s) are wrong
#   - the bisected culprit knob (a -fno-<pass> / inline / coalesce toggle that
#     restores the correct value), or "none" if no single knob isolates it
#   - a class: O0-WRONG (front-end/libc/codegen) · O1 · O2 · CRASH · COMPILE_CRASH
#
# Reproducers are saved to fuzz_triage_repros/.  tcc -O0 is normally CORRECT, so
# an O0-WRONG row points at the front end / libc / O0 codegen, not an optimizer.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
source "tests/fuzz/runseed.sh"   # provides runseed()

# Default JOBS = nproc-2 (leave 2 cores for the OS / qemu I/O threads).  Each
# worker is one single-threaded qemu, so throughput scales ~linearly with JOBS
# up to the core count: on a 32-core host nproc-2=30 measured ~1.7x over the old
# fixed default of 16.
_NPROC="$(nproc 2>/dev/null || echo 16)"
LO="${1:-0}"; HI="${2:-4999}"; JOBS="${3:-$(( _NPROC > 2 ? _NPROC - 2 : _NPROC ))}"
# Generator feature profile (Axis 2 of docs/plan_fuzz_reach_expansion.md).
# FUZZ_PROFILE=float sweeps the FP profile; default "int" = historical stream.
PROFILE="${FUZZ_PROFILE:-int}"
REPRO="$ROOT/tests/fuzz/fuzz_triage_repros"; mkdir -p "$REPRO"
if [ "$PROFILE" = int ]; then
  OUT="$ROOT/fuzz_triage_${LO}_${HI}.md"; SEEDPFX="seed"
else
  OUT="$ROOT/fuzz_triage_${PROFILE}_${LO}_${HI}.md"; SEEDPFX="${PROFILE}_seed"
fi

# High-value culprit knobs (curated from prior root causes — covers most).  Add
# more -fno-* flags here for a wider net; see `armv8m-tcc -fno-help`-style list
# in libtcc.c (dce/cse/const-prop/.../loop-unroll/loop-rotation/reroll-blocks).
KNOBS=(
  "-fno-const-prop" "-fno-copy-prop" "-fno-cse" "-fno-store-load-fwd"
  "-fno-dead-store-elim" "-fno-mla-fusion" "-fno-disp-fusion" "-fno-lea-fold"
  "-fno-jump-threading" "-fno-loop-unroll" "-fno-loop-rotation"
  "-fno-inline-functions|-fno-inline-small-functions"   # both: csmix inlining
  "ENV:TCC_NO_COALESCE=1"                                # graph coalescing
)

val() { echo "$1" | grep -oE "[0-9a-f]{8}|HardFault|Lockup|COMPILE_FAIL" | head -1; }

# Worker: print SEED iff tcc's O0/O1/O2/Os outputs are not all identical
# (self-contained; no pytest/xdist dependency).
sweep_one() {
  local s="$1" src; src="$(mktemp --suffix=.c)"
  # Emit exactly one status line per seed so the consumer's live counter always
  # reaches TOTAL — a seed we can't generate prints SKIP (counted, not divergent).
  python3 "$ROOT/tests/fuzz/gen_c.py" --seed "$s" --profile "$PROFILE" -o "$src" 2>/dev/null || { rm -f "$src"; echo "$s SKIP"; return; }
  local a b c d
  a="$(val "$(runseed "$src" -O0)")"; b="$(val "$(runseed "$src" -O1)")"
  c="$(val "$(runseed "$src" -O2)")"; d="$(val "$(runseed "$src" -Os)")"
  rm -f "$src"
  if [ "$a" = "$b" ] && [ "$a" = "$c" ] && [ "$a" = "$d" ]; then echo "$s OK"; else echo "$s FAIL"; fi
}

# Triage worker: regenerate one seed, get the gcc ground truth, run all four
# O-levels, classify, and bisect the culprit knob.  Prints exactly ONE markdown
# table row on stdout so callers can fan this out under xargs and sort the rows.
# Self-contained (uses exported runseed/val/KNOBS_STR) — no re-source per knob.
triage_one() {
  local s="$1"
  local src="$REPRO/${SEEDPFX}${s}.c"
  # Emit one line per seed even when generation fails (SKIP sentinel) so the live
  # "triaged N/NFAIL" counter completes; the consumer filters it from the table.
  python3 "$ROOT/tests/fuzz/gen_c.py" --seed "$s" --profile "$PROFILE" -o "$src" 2>/dev/null || { echo "SKIP $s"; return; }

  local gref ref=""
  gref="$(mktemp)"
  gcc -m32 -funsigned-char -O2 -w "$src" -o "$gref" 2>/dev/null \
    && ref="$("$gref" 2>/dev/null | grep -oE '[0-9a-f]{8}' | head -1)"
  rm -f "$gref"

  local o0 o1 o2 os
  o0="$(val "$(runseed "$src" -O0)")"; o1="$(val "$(runseed "$src" -O1)")"
  o2="$(val "$(runseed "$src" -O2)")"; os="$(val "$(runseed "$src" -Os)")"

  local cls="?" bad_lvl=""
  if [ "$o2" = "COMPILE_FAIL" ] || [ "$o1" = "COMPILE_FAIL" ]; then cls="COMPILE_CRASH"
  elif [ -n "$ref" ] && [ "$o0" != "$ref" ]; then cls="O0-WRONG"
  elif [ "$o1" != "$o0" ]; then cls="O1"; bad_lvl="-O1"
  elif [ "$o2" != "$o0" ]; then cls="O2"; bad_lvl="-O2"
  elif [ "$os" != "$o0" ]; then cls="Os"; bad_lvl="-Os"
  fi
  case "$o1$o2$os" in *HardFault*|*Lockup*) { [ "$cls" = "O1" ] || [ "$cls" = "O2" ]; } && cls="$cls/CRASH";; esac

  # bisect culprit at the bad level (skip for O0-WRONG / COMPILE_CRASH); call
  # runseed directly (it's exported) instead of re-sourcing runseed.sh per knob.
  local culprit="-" k kenv flags r
  if [ -n "$bad_lvl" ] && [ -n "$ref" ]; then
    for k in $KNOBS_STR; do
      kenv=""; flags="$k"
      [[ "$k" == ENV:* ]] && { kenv="${k#ENV:}"; flags=""; }
      flags="${flags//|/ }"
      if [ -n "$kenv" ]; then
        r="$(export "$kenv"; val "$(runseed "$src" $bad_lvl)")"
      else
        r="$(val "$(runseed "$src" $bad_lvl $flags)")"
      fi
      if [ "$r" = "$ref" ]; then culprit="${k//|/ +}"; [ -n "$kenv" ] && culprit="$kenv"; break; fi
    done
  fi

  printf '| %s | %s | %s | %s | %s | %s | %s | %s |\n' \
    "$s" "$cls" "${ref:-?}" "${o0:-?}" "${o1:-?}" "${o2:-?}" "${os:-?}" "$culprit"
}
export -f sweep_one triage_one val
export REPRO PROFILE SEEDPFX
export KNOBS_STR="${KNOBS[*]}"   # arrays don't survive `export -f`; pass as a string

# 1) enumerate failing seeds (unless an explicit SEEDS list was given)
if [ -n "${SEEDS:-}" ]; then
  FAILS="$(echo "$SEEDS" | tr ' ' '\n' | sort -un)"
elif [ -n "${FAST_SWEEP:-}" ]; then
  # Opt-in fast pre-scan: batch_sweep.py packs many seeds into ONE qemu boot
  # (~4*ceil(N/batch) boots instead of 4N).  It is ~2-4x faster but LOSSY — it
  # misses context-sensitive miscompiles (~1 in 5; see batch_sweep.py header).
  # Use for rapid iteration; run without FAST_SWEEP to certify a range clean.
  echo "FAST_SWEEP: batched pre-scan $LO-$HI (lossy — misses context-sensitive bugs)..." >&2
  FAILS="$(python3 "$ROOT/tests/fuzz/batch_sweep.py" "$LO" "$HI" --jobs "$JOBS" | sort -un)"
else
  TOTAL=$((HI - LO + 1))
  echo "Sweeping olevels $LO-$HI ($TOTAL seeds) across $JOBS workers..." >&2
  # sweep_one prints "<seed> OK|FAIL"; tally live and collect the FAILs.
  FAILS="$(seq "$LO" "$HI" | xargs -P "$JOBS" -I{} bash -c 'sweep_one {}' \
    | { done=0; fail=0;
        while read -r seed st; do
          done=$((done + 1))
          if [ "$st" = FAIL ]; then fail=$((fail + 1)); echo "$seed"; fi
          printf '\r  swept %d/%d  (%d%%)  divergent=%d   ' \
                 "$done" "$TOTAL" $((done * 100 / TOTAL)) "$fail" >&2
        done
        printf '\n' >&2
      } | sort -un)"
fi
NFAIL="$(echo "$FAILS" | grep -c .)"
[ "$NFAIL" -gt 0 ] || { echo "No O-level divergences in $LO-$HI (all opt levels agree)."; exit 0; }
echo "Triaging $NFAIL divergent seed(s)..." >&2

{
  echo "# Fuzz O-level triage  ($LO-$HI)"
  echo
  echo "Ground truth = \`gcc -m32 -funsigned-char\`.  tcc -O0 is normally correct."
  echo
  echo "| seed | class | ref | O0 | O1 | O2 | Os | culprit knob |"
  echo "|------|-------|-----|----|----|----|----|--------------|"
} > "$OUT"

# Triage every failing seed IN PARALLEL across $JOBS workers (the slow part: each
# seed is up to ~17 compile+run cycles, so a serial loop here ran ~10x slower than
# the parallel sweep).  Each worker prints one markdown row; tally live, then sort
# the rows by seed and append.  Rows are collected via stdout — no concurrent
# appends to $OUT — so the table can't interleave or corrupt.
echo "$FAILS" | xargs -P "$JOBS" -I{} bash -c 'triage_one "$1"' _ {} \
  | { done=0
      while IFS= read -r row; do
        done=$((done + 1))
        printf '\r  triaged %d/%d   ' "$done" "$NFAIL" >&2
        case "$row" in '|'*) echo "$row";; esac   # drop SKIP sentinels from the table
      done
      printf '\n' >&2
    } | sort -t'|' -k2 -n >> "$OUT"

echo >> "$OUT"
echo "Repros in tests/fuzz/fuzz_triage_repros/.  Per-seed serial repro:" >> "$OUT"
echo '`python3 scripts/diff_olevels.py --seed N --require-qemu`' >> "$OUT"
echo "Report written to $OUT" >&2
