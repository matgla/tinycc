# Plan: Expanding fuzz reach for the ARMv8-M tinycc

> Companion to the two existing fuzz docs, which cover **how to triage and fix**
> a divergence once found:
> - [`docs/fuzz_triage_guide.md`](fuzz_triage_guide.md) — the sweep/triage cadence.
> - [`docs/debugging_fuzz_divergences.md`](debugging_fuzz_divergences.md) — root-cause
>   workflow (`bisect_opt.py`, culprit knobs, IR dumps).
>
> This doc is about the **other half**: making the fuzzer *reach* bug classes it
> currently cannot. Triage tooling is mature; the limiter now is **what the
> generator samples**.

## Why expand reach (the leverage argument)

The differential fuzzer has already paid for itself: 28 distinct root causes are
now regression-locked as `tests/ir_tests/188_*` … `215_*`, each with a fuzz
memory documenting the fix. But it is approaching a plateau, because it samples a
**narrow slice of C along two axes only**:

1. **Seed range** — swept exhaustively to ~9999, then it stops.
2. **Feature space** — [`tests/fuzz/gen_c.py`](../tests/fuzz/gen_c.py) emits *only*
   UB-free integer programs: types `int/unsigned/char/short/long`, fixed-size
   arrays, 3-field structs, helper functions, `+ - * / % << >> == cond`, and
   bounded loops with calls. That is the entire alphabet.

Every fix so far lives inside that slice. The **richest remaining bug seams are
literally unreachable** by the current generator — and we know where they are,
because the historical bug record (the fuzz memories + `failing_tests.txt`) is
dominated by features the generator never emits:

| Bug class in the historical record | Generator emits it? |
|---|---|
| softfloat / FP (`dadd` GRS rounding, `ldexp`, `strtod`, `22_float`, `%f=0`) | **no — no floats at all** |
| sret / struct-by-value return (`131`, inline sret, indirect-call sret stack-arg) | **no — structs never returned/passed by value** |
| packed structs + bitfields (`#pragma pack`, `long long z:63` @-O2, `100_c99array`) | **no — no bitfields, no `packed`** |
| function pointers / indirect calls (fn-ptr dispatch, sret via reg-deref) | **no — no function pointers** |
| `switch`/`goto` / jump tables (`SWITCH_TABLE` BFS, dead_loop switch-bounds) | **no — no `switch`, no `goto`** |
| varargs / `stdarg` (`stdarg-2` frame-shrink, the `va-arg-*` -O0 failures) | **no — no varargs** |
| pointer aliasing (`sl_forward`, DSE, `load_cse`, deref-forward — *many* memos) | **no — "no pointer/aliasing tricks" by design** |

**Conclusion:** range expansion is cheap and worth doing, but the high-order win
is **feature-space expansion** — each new UB-safe construct unlocks a whole bug
class the optimizer has never been fuzzed against. This plan orders the work by
expected bug density, not by ease.

---

## Axis 1 — Seed range (cheap; do first, it primes the pump)

The 10000+ range is **entirely unswept**. The fast pre-scan makes a six-figure
sweep affordable (~200 seeds per qemu boot vs. one boot per seed):

```bash
cd libs/tinycc && make cross -j$(nproc)

# Fast pre-scan a large frontier (batch_sweep: ~200 seeds/boot, JOBS=nproc-2)
FAST_SWEEP=1 python3 tests/fuzz/batch_sweep.py 10000 50000 > /tmp/frontier.txt

# Triage ONLY the flagged seeds exhaustively (real culprit bisect + repros)
SEEDS="$(cat /tmp/frontier.txt)" tests/fuzz/triage_olevels.sh
```

**Recall caveat (must respect):** `batch_sweep` is a *pre-scan, ~80% recall*. It
runs each seed one call-frame deep inside a runner, so it misses the
**context-sensitive uninit-read class** — miscompiles that only diverge under
crt0's exact entry stack/register state (e.g. seed 8300). Context-*insensitive*
bugs (const-prop, dead-store, loop-unroll, jump-threading, COMPILE_CRASH) are
caught. Therefore:

- Use `batch_sweep` for **rapid frontier scanning**, never to *certify* a range.
- To **certify a band clean**, run the exhaustive per-seed oracle:
  `tests/fuzz/triage_olevels.sh LO HI` (one standalone crt0 boot per seed/level).
- Define a rolling **certified band** (start: `0–9999` exhaustive) that must stay
  green, and an **advancing frontier** (pre-scan + triage-on-flag) that grows it.

---

## Axis 2 — Generator feature space (highest leverage)

Add new UB-safe constructs to `gen_c.py`, **one axis at a time**, ranked below by
historical bug density. Each axis names: the UB-freedom invariants to preserve
(so divergence always means a real bug, never a legal disagreement), the bug
class it unlocks, and the oracle that catches it.

> **Hard design constraint — versioning the generator.**
> Generation is one deterministic `random.Random(seed)` stream. Adding *any* new
> emission kind reshuffles the stream, so **every seed number changes meaning** —
> which would rot the seed→bug mapping in the triage tables and the fuzz memories.
> Materialized repros are safe (`tests/fuzz/fuzz_triage_repros/seed*.c` and
> `ir_tests/188–215` are frozen `.c` files), but seed *numbers* are not.
> **Do not edit the default stream.** Expand via an explicit, additive
> **generator profile** dimension — e.g. `gen_c.py --profile float` /
> `--profile ptr` (default profile = today's stream, byte-identical). Triage
> tables then key on `(profile, seed)`. This keeps the existing corpus valid and
> lets each feature class be swept and certified independently.

Ranked axes (do top-down — stop and harvest fixes before opening the next):

1. **Floating point (`float`/`double`, softfloat). ✅ LANDED — `--profile float`.**
   Densest seam in the record, and it paid off immediately (see *Status* below).
   Implementation in `gen_c.py`:
   - Exact, finite, normal literals (hex-float, mantissa < 2²⁴ → bit-identical in
     float *and* double, so no parser-rounding false positives); no `NaN`/`Inf`
     (divisor forced nonzero; results clamped to |x| ≤ 2⁴⁰ so loop-carried FP
     can't grow to Inf).
   - **Three-address single ops** (`fd = fa OP fb`) — no `a*b+c` to contract, and
     every intermediate is rounded to nominal precision → portable across both
     ARM soft-float oracles.
   - Folded into the integer checksum by **memcpy bit-reinterpret** (no aliasing
     UB; the store rounds away any excess precision).
   - Oracle: the board is `-mfloat-abi=soft`, so **both** tcc and gcc use IEEE
     correctly-rounded soft-float (no x87 excess precision, no FMA) → olevels
     self-consistency *and* vs-gcc-arm are both bit-exact and sound.
   - ⚠ The *x86* `gcc -m32` ground-truth column in `triage_olevels.sh` can carry
     x87 excess precision for FP — treat it as advisory; trust the ARM oracles
     (tcc-O0 is the de-facto correct baseline, confirmed by arm-gcc).

   **Status (first run, 2026-06-29):** of float seeds 0–8, **7/8 diverge across
   tcc O-levels**, all gold-confirmed real (`arm-none-eabi-gcc -O0/-O2 == tcc-O0`
   ≠ `tcc-O1/-O2`). Seeds 0–3 all bisect to culprit knob **`-fno-const-prop`** —
   a strong single-root-cause lead (tcc const-prop mishandling FP). Handed to the
   fix track. Remaining axes (2–7) still TODO.

   Sweep it with the existing infrastructure (profile flows through all of them):
   ```bash
   FUZZ_PROFILE=float tests/fuzz/triage_olevels.sh 0 999      # -> fuzz_triage_float_0_999.md
   python3 tests/fuzz/batch_sweep.py 0 5000 --profile float   # fast pre-scan (FP bugs ARE caught)
   FUZZ_PROFILE=float FUZZ_OLEVEL_SEEDS=0-199 python3 -m pytest tests/fuzz/test_random_c_olevels.py -q
   FUZZ_PROFILE=float FUZZ_VSGCC_SEEDS=0-199  python3 -m pytest tests/fuzz/test_random_c_vs_gcc.py  -q
   python3 tests/fuzz/gen_c.py --seed N --profile float        # inspect one program
   ```

2. **Function pointers / indirect calls.** Build a small static dispatch table of
   the already-emitted helpers; call through `tab[idx & (N-1)]`. UB-safe (all
   targets have compatible prototypes, index masked). Unlocks: indirect-call ABI,
   sret-through-fn-ptr, stack-arg delivery, fn-ptr CSE/devirt. Oracle: vs-gcc.

3. **Bitfields & packed structs.** Emit `struct { unsigned a:N; … }` and
   `#pragma pack`/`__attribute__((packed))` variants; read/write fields into the
   checksum. UB-safe (unsigned bitfields, in-range values). Unlocks: bitfield
   load/store width, packed-member unaligned access, `known_bits` wide-store.
   Oracle: vs-gcc (bit layout) + olevels.

4. **`switch` / `goto` / labels.** Emit dense and sparse `switch` (drives
   `SWITCH_TABLE`) and forward `goto` over bounded regions. UB-safe (bounded,
   every path initializes). Unlocks: jump-table codegen, switch-bounds vs. loop
   confusion, BFS over case targets. Oracle: olevels.

5. **struct/union by value (params + return / sret).** Pass and `return` structs
   by value; add small unions (write-then-read the same member — no type-punning,
   stays UB-free). Unlocks: AAPCS struct passing, sret copies, union layout.
   Oracle: vs-gcc.

6. **varargs.** A single generated `int vsum(int n, ...)` summing `n` args into
   the checksum. UB-safe (count matches, all `int`). Unlocks: `stdarg` frame
   layout — directly relevant to the `va-arg-*` -O0 failures in
   `failing_tests.txt`. Oracle: vs-gcc.

7. **Restricted pointers / aliasing.** The current generator bans pointers
   entirely "to stay UB-free". Reintroduce them *carefully*: only `&local`,
   in-bounds `&arr[i]`, single-level deref, no type-punning, no escaping. This is
   the biggest aliasing-bug surface (`sl_forward`, DSE, `load_cse`,
   deref-forward, offset-0 chained-store — a large fraction of the memo record),
   so it's high value but needs the strictest UB discipline. Oracle: olevels.

For each axis, add a focused pytest selector (`FUZZ_OLEVEL_SEEDS` + a profile
env) and a triage table `fuzz_triage_<profile>_<lo>_<hi>.md`, mirroring the
integer-profile flow.

---

## Axis 3 — Oracle expansion (find the O0-WRONG class)

The olevel oracle assumes **tcc -O0 is correct** and only flags O1/O2/Os
disagreement. It is structurally blind to **frontend / libc / O0-codegen** bugs
where *all* tcc levels agree but are wrong. We have live evidence this class is
non-empty: `failing_tests.txt` shows 31 gcc-torture failures **at `-O0`**
(`stdarg-*`, `va-arg-*`, `ieee/copysign*`, `vprintf-1`, …).

Only the **tcc-vs-gcc** oracle catches O0-WRONG, and today it runs on just a
12-seed pytest default
([`tests/fuzz/test_random_c_vs_gcc.py`](../tests/fuzz/test_random_c_vs_gcc.py),
ground truth `arm-none-eabi-gcc -O2` under the same QEMU board). Expand it to a
**first-class swept oracle**:

```bash
# Sweep the vs-gcc differential over a range (already supports FUZZ_VSGCC_SEEDS)
FUZZ_VSGCC_SEEDS=0-9999 python3 -m pytest tests/fuzz/test_random_c_vs_gcc.py -n 16 -q
# Torture-corpus mode for breadth:
python3 scripts/diff_vs_gcc.py --mode torture
```

Wire-up work:
- Have `triage_olevels.sh` emit an **`O0-WRONG`** row when O0 itself diverges from
  the `gcc -m32 -funsigned-char` ground truth, and auto-escalate that seed to the
  vs-gcc harness for confirmation.
- This makes the vs-gcc oracle the generative complement to `failing_tests.txt`:
  the torture suite is a *fixed* O0-WRONG corpus; the fuzzer mines *new* ones —
  most valuable once Axis 2 adds floats/varargs (the exact failing features).

---

## Axis 4 — Optimization pressure (secondary)

Today's pressure is just `-O0/-O1/-O2/-Os`. Cheap additions, lower priority than
Axes 1–3:
- Sweep a few **`-fno-<pass>` combinations** and `TCC_DISABLE_PASS` / `TCC_SKIP_SSA`
  configs as extra "levels" in the self-consistency oracle — surfaces bugs that
  only appear in specific pass orderings (the playbook already uses these knobs
  for bisection; here they become *finders*, not just *localizers*).
- Add `-Os` size-sensitive layouts to the vs-gcc oracle (literal-pool / branch
  narrowing bugs are layout-dependent — cf. tests 192, 207).

---

## CI / cadence

```
┌─ certified band (exhaustive, GATE) ──────────────────────────────┐
│ triage_olevels.sh 0 9999   +  vs-gcc 0-N   →   must stay 0 fails  │  per-commit / nightly
├─ advancing frontier (pre-scan + triage-on-flag) ─────────────────┤
│ batch_sweep 10000 → +10k each run; triage flagged; grow the band │  nightly
├─ per-profile sweeps (Axis 2) ───────────────────────────────────┤
│ one band per generator profile as each lands (float, ptr, …)     │  on profile add
└──────────────────────────────────────────────────────────────────┘
```

Every confirmed root cause follows the existing fix→verify→lock loop
([`docs/fuzz_triage_guide.md`](fuzz_triage_guide.md)) and lands a regression test
at `tests/ir_tests/NN_fuzz_<cause>.c` (+`.expect` = the `gcc -m32 -funsigned-char`
value), registered in `test_qemu.py` — continuing the 188–215 sequence.

---

## Definition of done

- A documented **certified band** that the CI gate keeps green (start 0–9999), and
  a frontier that advances it.
- The **vs-gcc oracle** runs as a swept gate (not a 12-seed smoke), with O0-WRONG
  escalation wired into triage.
- At least the **top-3 generator profiles** (float, function-pointer, bitfield)
  landed behind `--profile`, each with its own swept+certified band.
- A single **canonical status tracker** replacing the two stale root-level tables
  (`fuzz_triage_0_10000.md`, `fuzz_triage_2000_10000.md`) — keyed on
  `(profile, seed) → class → root cause → fix commit → regression test`.

---

## Appendix — reconcile before expanding (current open backlog)

The two root-level triage tables are **stale**: they list seeds 3210 and 3691 as
open `O1`, but both are fixed (tests 214, 215) per the fuzz memories. Before
opening new frontier, run one authoritative exhaustive sweep and rebuild a single
table. Best-known open set in 0–9999 (from the tables, minus memory-confirmed
fixes), clustered by culprit knob:

| seed | class | culprit knob (root) |
|---|---|---|
| 4482, 5656, 6214, 6447, 9403 | O1 | `-fno-const-prop` (likely one shared root) |
| 6951 | O2 | `-fno-jump-threading` |
| 8985 | O2 | `-fno-loop-unroll` |
| 4193, 4594, 7918 | O1 | none isolates (SSA-pipeline — use `TCC_SKIP_SSA`) |
| 8078 | COMPILE_CRASH | compiler asserts (not a miscompile) |

Group the `-fno-const-prop` cluster first — one fix likely clears several, as the
2698-batch did (5689/8300/8606).

```bash
# authoritative re-sweep + fresh canonical table
tests/fuzz/triage_olevels.sh 0 9999        # writes fuzz_triage_0_9999.md
```
