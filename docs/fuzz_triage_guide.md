# Fuzz-sweep & triage guide

How to enumerate, triage, and fix the remaining O1/O2 wrong-code bugs the
differential fuzzer finds, using the same workflow that cleared seeds 0–299.

## TL;DR — the 45-minute cadence you asked for

```bash
cd libs/tinycc
make cross -j$(nproc)                       # ensure armv8m-tcc is current

# ── ~15 min: sweep + triage a wide range ───────────────────────────────
tests/fuzz/triage_olevels.sh 0 4999 24      # LO HI JOBS  -> fuzz_triage_0_4999.md
#   (self-contained; a 5000-seed sweep is a few minutes on ~24 cores, then a
#    quick per-seed culprit bisect on the handful that diverge)

# ── ~30 min: iterate on fixes ──────────────────────────────────────────
#   open fuzz_triage_0_4999.md, fix highest-leverage culprit groups first,
#   rebuild + verify per below, run the regression gate, repeat.
```

`triage_olevels.sh` writes a markdown table classifying every failing seed and
bisecting a culprit pass. Reproducers land in `tests/fuzz/fuzz_triage_repros/`.
The sweep is **self-contained** — pure bash + `xargs -P` over `runseed.sh`, no
`pytest`/`pytest-xdist` dependency (so it works regardless of the active venv).

## Re-checking only the seeds that failed

Sweeping a band to answer "did my change break anything *new*?" is almost all
waste: you already know which seeds diverge, and re-running the other 5,999 only
confirms that. `batch_sweep.py --seeds` takes an explicit list and skips the band
entirely — the 25 known-divergent seeds of `sweep_all.py 0 6000` re-check in
**under a second**, versus ~65 s for the full band and far longer for a triage
sweep.

```bash
# One profile, one explicit list.  --olevels needs the "=" form: argparse
# otherwise eats the leading "-O0" as a flag.
python3 tests/fuzz/batch_sweep.py --profile ptr \
        --seeds "1,1410,1450,2107" --olevels="-O0,-O1,-O2,gcc-O0" --no-cache
```

Output is the tagged form — `OLEVELS` (tcc levels disagree with each other),
`VSGCC` (they agree but differ from the gcc reference), `GCCBAD` (gcc itself is
inconsistent). Seeds are per-profile, so keep one list per profile: seed 96 of
`combo` and seed 96 of `ptr` are different programs.

The point of the re-check is the **A/B**, not the absolute count. A sweep report
listing failing seeds means nothing on its own — those seeds may have been
failing for months. Two ways to establish that, cheapest first:

```bash
# 1. Kill switch, if the change has one (no rebuild):
TCC_NO_SCRATCH_DEMOTE=1 python3 tests/fuzz/batch_sweep.py --profile ptr \
        --seeds "$SEEDS" --olevels="-O0,-O1,-O2,gcc-O0" --no-cache

# 2. Against HEAD.  Now affordable, because the sweep itself is ~1 s:
git stash -- <the files you changed> && make cross -j$(nproc)
python3 tests/fuzz/batch_sweep.py --profile ptr --seeds "$SEEDS" \
        --olevels="-O0,-O1,-O2,gcc-O0" --no-cache
git stash pop && make cross -j$(nproc)
```

Identical seed sets = pre-existing, and the change is clear. A seed that
diverges only with the change on is yours: reduce it per
`docs/debugging_fuzz_divergences.md`.

Two caveats. `batch_sweep.py` is the ~80%-recall pre-scan (it runs each seed one
call frame deep, so miscompiles that depend on crt0's exact entry state hide) —
a clean seed-list re-check is evidence about *those seeds*, not a certification
of the band, so a wide sweep still has to run once. And `--no-cache` matters:
without it a cached result from the previous build can be served back and the
A/B compares a binary against itself.

## Prerequisites

- `make cross` built `armv8m-tcc` (rebuild after any compiler change).
- `gcc` with 32-bit multilib (`gcc -m32`) — the ground-truth oracle.
- `qemu-system-arm`, `arm-none-eabi-gcc`.
- The mps2 newlib is built on first IR-test run; if missing:
  `sh tests/ir_tests/qemu/mps2-an505/build_newlib.sh`.
- `pytest` (+ `pytest-xdist` for `-n`) is needed only for the **regression
  gate** below, not for the sweep.

## Why these oracles

- **O-level self-consistency**: `triage_olevels.sh` compiles each seed at O0,
  O1, O2, Os on the *same* ARM target and flags any disagreement (the same
  contract as `tests/fuzz/test_random_c_olevels.py`, but standalone). No ABI
  mismatch, fully reproducible — this is the authoritative sweep.
- **Ground truth = `gcc -m32 -funsigned-char`.** ARM's ABI is *unsigned* `char`
  + *32-bit* `long`; plain `gcc`/`gcc -m32` (signed char) mis-judges any program
  that uses `char`, which made O0 look wrong last time. Always pass both flags.
- **tcc -O0 is (so far) always correct** — so an optimizer is to blame whenever
  O1/O2/Os diverge from O0. A row classed `O0-WRONG` instead points at the front
  end / libc / O0 codegen (rare; investigate separately).

## Reading the triage report

| column | meaning |
|--------|---------|
| `class` | `O1` / `O2` / `Os` = that level miscompiles · `…/CRASH` = HardFault/Lockup · `COMPILE_CRASH` = compiler asserted (e.g. `mach_get_dest_reg: unexpected kind 3`, seed 2966) · `O0-WRONG` = not an optimizer bug |
| `ref` | gcc -m32 -funsigned-char result (the correct value) |
| `O0..Os` | tcc output per level |
| `culprit knob` | the single `-fno-<pass>` / `TCC_NO_COALESCE` that restores `ref`, or `-` if none isolates it |

Group rows by `culprit knob` — one root cause usually covers several seeds (last
batch: 4 seeds shared `ssa_opt_dead_loop`, 2 shared `local_alu_cse`, etc.).

## Fix → verify loop (per bug)

```bash
S=588; LVL=-O2                                   # from the report
python3 tests/fuzz/gen_c.py --seed $S -o /tmp/s$S.c
# 1. confirm + ground truth
gcc -m32 -funsigned-char -O2 -w /tmp/s$S.c -o /tmp/g && /tmp/g     # correct value
bash tests/fuzz/runseed.sh /tmp/s$S.c $LVL                          # tcc value (wrong)
# 2. find the diverging statement: insert a trace after each `cs = csmix(...)`:
perl -pe 's/(cs = csmix\([^;]*\);)/$1 trace(__LINE__,cs);/g' /tmp/s$S.c > /tmp/t.c
perl -0pi -e 's/(#include <stdio.h>)/$1\nstatic void trace(int l,unsigned v){printf("L%d=%08x\\n",l,v);}/' /tmp/t.c
gcc -m32 -funsigned-char -O0 -w /tmp/t.c -o /tmp/g && /tmp/g > /tmp/ref.txt
# ...run /tmp/t.c through the mps2 makefile at $LVL, diff vs /tmp/ref.txt -> first divergent line
# 3. dump IR around it (debug build):  ./armv8m-tcc -dump-ir-passes=all $LVL -c /tmp/s$S.c -o x.o 2>/dev/null
# 4. edit the implicated pass, then:
make cross -j$(nproc)
bash tests/fuzz/runseed.sh /tmp/s$S.c $LVL        # == ref ?  (also re-check O0/O1/O2/Os)
```

### When `culprit knob = none`

The `-fno-*` flags only gate the `opt.c` pipeline. SSA-pipeline bugs (and the
SSA *rename* itself, e.g. the multidef-temp ternary) won't isolate. Temporarily
add skip gates to the two SSA drivers, rebuild, then bisect with
`TCC_SKIP_SSA="ssa:gvn"` / `TCC_SKIP_SSA2="ssa:cprop"`:

```c
// ir/regalloc.c  — RUN_SSA macro
const char *skip__ = getenv("TCC_SKIP_SSA");
if (!(skip__ && strstr(skip__, name))) { (call); }    // wrap the (call);
// ir/opt/ssa_opt.c — SSA_RUN macro
const char *skip2__ = getenv("TCC_SKIP_SSA2");
if (!(skip2__ && strstr(skip2__, name))) changes += (call);
```
Pass names: `ssa:var_const_fold ssa:var_forward ssa:sccp ssa:cprop ssa:fold
ssa:gvn ssa:reassoc ssa:strength ssa:narrow ssa:dce ssa:dead_loop ...`. If
*no* SSA-skip and *no* `-fno` helps but `-fno-inline-functions
-fno-inline-small-functions` does, the bug is exposed by inlining (the
multidef-temp class). **Remove these gates before committing.**

## Regression gate (run before committing any fix)

```bash
cd libs/tinycc
FUZZ_OLEVEL_SEEDS=0-299 python3 -m pytest tests/fuzz/test_random_c_olevels.py -n 16 -q   # must stay 300/300
cd tests/ir_tests
python3 -m pytest test_qemu.py test_codegen_asm.py \
        test_gcc_torture_ir.py -k "O1 or O2 or not torture" -n 8 -q                       # was 9063/0
cd ../unit && make clean && make run                                                      # 1116/0
```
Add a regression test for each fix: a verbatim repro at `tests/ir_tests/NN_fuzz_<cause>.c`
+ `.expect` (the gcc -m32 -funsigned-char value) registered in `test_qemu.py`
(see 188–195 for the pattern). **Watch for size-sensitive tests** like
`96_nodata_wanted` (labels-as-values) when a fix changes codegen layout.

## Parallelizing the diagnosis (optional, fast)

For a big batch, ask Claude to run the **diagnosis workflow**: one agent per
failing seed reduces + root-causes it in parallel (read-only, no rebuilds, using
this same `runseed.sh` + the knobs), returning a grouped root-cause report. That
turned the 12-seed batch around in one pass; you then apply fixes serially.

## Current known batch (300–2999, as of this writing)

~25 failing seeds. Notable: **2966** = `COMPILE_CRASH`
(`mach_get_dest_reg: unexpected kind 3`); **588** = O2, culprit `-fno-const-prop`.
Full list — rerun `tests/fuzz/triage_olevels.sh 300 2999 24`.
0–299 is clean (300/300).
