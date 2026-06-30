# Plan: varargs profile — fuzz stdarg frame layout via a generated `unsigned vsum(unsigned n, ...)`

> Instantiates §2.3 of [`plan_fuzz_coverage_master.md`](plan_fuzz_coverage_master.md)
> for row [§3.5](plan_fuzz_coverage_master.md). **Profile name:** `varargs`.
> **Feature flag:** `"varargs"`. Purely additive to
> [`gen_c.py`](../tests/fuzz/gen_c.py); the default `int` stream stays
> byte-identical (it draws no new `rng` value and emits no new text when the flag
> is absent, exactly as the `float` profile does).

```
## Profile: varargs           feature flag: "varargs"
UB invariants     : arg count == n exactly · all variadic args a single non-promotable type (unsigned/int) · masked values · va_end always called
Emission shapes   : static unsigned vsum(unsigned n, ...) summing n int args via va_start/va_arg/va_end → cs; call sites with VARYING arg count straddling the 4-register AAPCS boundary
Bug class unlocked: stdarg frame layout, variadic r0-r3 spill + stack-arg delivery, the va-arg-* / stdarg-* O0-WRONG failing class
Oracle            : vs-gcc (ABI surface; only oracle that sees O0-WRONG) + olevels for opt stability
Sweep             : FUZZ_PROFILE=varargs tests/fuzz/triage_olevels.sh 0 999
                    python3 tests/fuzz/batch_sweep.py 0 5000 --profile varargs
Certify           : exhaustive 0–N green vs-gcc + olevels
Harvest           : ir_tests/NN_fuzz_<cause>.c (+.expect) per root cause, + memory
```

---

## 1. Goal

Make the fuzzer emit **variadic functions and calls to them** so the differential
oracle exercises the entire `stdarg` lowering path — the prologue spill of the
incoming `r0–r3` into a contiguous save area, the `__gr_top` / named-arg-bytes
bookkeeping that `va_start` reads, and the word-by-word walk that `va_arg`
performs across the register-saved area *and* into the caller's stack arguments —
none of which the `int`/`float`/`fnptr` profiles ever reach. The profile emits a
**single** generated helper `static unsigned vsum(unsigned n, ...)` that sums `n`
`int` arguments, and calls it from `main()` with a **varying number of
arguments** so call sites straddle the 4-register AAPCS boundary into stack args.
Every `vsum(...)` result folds into the rolling `cs` checksum, so a wrong frame
layout or a misread arg changes the printed output.

---

## Status — IMPLEMENTED (2026-06-30) · band CLEAN

The `varargs` profile is **landed in [`gen_c.py`](../tests/fuzz/gen_c.py)** and validated:

- **M0–M4 done.** All 6 prior streams (int/float/fnptr/bitfield/switch/struct_byval)
  byte-identical (seeds {0,1,2,7,42,100,1000}); `arm-none-eabi-gcc -O0/-O1/-O2 -Wall -Wextra`
  compile-clean over seeds 0–40 (0 err / 0 warn). One fixed `vsum(unsigned n, ...)` in the
  prologue (gated `#include <stdarg.h>`), `vcall` statement with a varying arg count (count == n
  by construction; `k>=4` spills past r0–r3 onto the stack), int-only variadic args, mandatory
  fold so `vsum` is never unused.
- **M5–M6 done — CLEAN band.** olevels pre-scan **0 divergent / 5001**; vs-gcc gold gate
  **1000/1000 over 0–999**. The single-named-arg + all-`int` variadic shape is correct in this
  fork; the historical `va-arg-*` O0-WRONG failures use mixed/promotable-type shapes this
  UB-safe profile deliberately avoids (§9).
- **No harvest needed** (clean). **Recommended follow-up:** a sibling shape that varies the
  number of *named* leading args before `...` (changing the r0–r3 split point) to probe the
  named-byte bookkeeping (`__gr_top`/`named_reg_bytes`) more aggressively.

---

## 2. Bug class unlocked

Variadic functions take a backend path that **no fixed-arity call reaches**, and
the master plan flags it as directly the `va-arg-*` / `stdarg-*` **O0-WRONG**
failing class (master plan [§3.5](plan_fuzz_coverage_master.md), and the reach
argument in
[`plan_fuzz_reach_expansion.md:38`](plan_fuzz_reach_expansion.md#L38) /
[`:156`](plan_fuzz_reach_expansion.md#L156) /
[`:178`](plan_fuzz_reach_expansion.md#L178), which names `stdarg-2` frame-shrink
and the `va-arg-*` -O0 failures). Concrete seams:

- **The `...` parse → `func_var` flag.** The ellipsis sets `FUNC_ELLIPSIS`; the
  global variadic flag is `ST_DATA int func_var` declared at
  [tccgen.c:167](../tccgen.c#L167), set per function at
  [tccgen.c:17328](../tccgen.c#L17328) and
  [tccgen.c:29190](../tccgen.c#L29190). Everything below keys off `func_var`.
- **Prologue r0–r3 spill (the heart of the frame layout).** For a variadic
  callee the backend pushes `r0–r3` *first* so they are contiguous with the
  caller's stack args, then records the bookkeeping `va_start` needs. See
  [arm-thumb-gen.c:216](../arm-thumb-gen.c#L216) (`int vararg_push_size`),
  the push + `vararg_push_size = 16` at
  [arm-thumb-gen.c:9538](../arm-thumb-gen.c#L9538) /
  [arm-thumb-gen.c:9604](../arm-thumb-gen.c#L9604), the
  `offset_to_args` adjustment at
  [arm-thumb-gen.c:9561](../arm-thumb-gen.c#L9561) /
  [arm-thumb-gen.c:9611](../arm-thumb-gen.c#L9611), and the named-arg bookkeeping
  block ([arm-thumb-gen.c:9676](../arm-thumb-gen.c#L9676)) that stores `r0–r3` at
  `FP-16..FP-4`, `__gr_top` at `FP-20`, `named_reg_bytes` at `FP-24`, and
  `named_stack_bytes` at `FP-28` ([arm-thumb-gen.c:9683](../arm-thumb-gen.c#L9683)).
  The epilogue then unwinds `vararg_push_size`
  ([arm-thumb-gen.c:9995](../arm-thumb-gen.c#L9995)–[10036](../arm-thumb-gen.c#L10036)).
- **Named-arg byte tracking in IR.** The exact FP-24 / FP-28 values come from
  `ir->named_arg_reg_bytes` / `ir->named_arg_stack_bytes`
  ([tccir.h:453](../tccir.h#L453)), computed in
  [ir/core.c:790](../ir/core.c#L790)–[828](../ir/core.c#L828) while classifying
  the *named* parameters. `vsum`'s single named `unsigned n` consumes 4 bytes of
  `r0`, so the anonymous args begin at `r1` — exercising the partial-register
  case (not the all-on-stack or all-in-r0 corner).
- **`va_start` runtime.** `__builtin_va_start(ap, ...)` expands to
  `__tcc_va_start(&(ap), __builtin_frame_address(0))`
  ([include/tccdefs.h:325](../include/tccdefs.h#L325)); `__tcc_va_start`
  ([lib/va_list.c:86](../lib/va_list.c#L86)) reads `gr_top`/`reg_bytes`/
  `named_stack_bytes` from the FP-relative slots above and points `ap` at the
  first anonymous argument.
- **`va_arg` lowering + runtime.** ARM `__builtin_va_arg` is a compiler
  intrinsic, lowered at [tccgen.c:22685](../tccgen.c#L22685)–[22793](../tccgen.c#L22793)
  to a `__tcc_va_arg(&ap, size, align)` IR call
  ([tccgen.c:22732](../tccgen.c#L22732)); the runtime
  ([lib/va_list.c:104](../lib/va_list.c#L104)) aligns `ap`, rounds the size to a
  word, and advances — the exact pointer arithmetic the `va-arg-*` failures break.
- **`va_list` type.** ARM EABI `va_list` is a plain `char *`
  ([include/tccdefs.h:320](../include/tccdefs.h#L320)), surfaced via
  [include/stdarg.h:4](../include/stdarg.h#L4)–[8](../include/stdarg.h#L8).
- **Variadic AAPCS classification (call site).** The caller classifies named vs.
  anonymous args via `tcc_abi_classify_argument`
  ([arch/arm/arm_aapcs.c:26](../arch/arm/arm_aapcs.c#L26)); the comment at
  [arch/arm/arm_aapcs.c:99](../arch/arm/arm_aapcs.c#L99) notes anonymous variadic
  args are passed **by value** (not invisible reference). Call-site variadic
  detection: [tccgen.c:15674](../tccgen.c#L15674).
- **Default argument promotion.** `gfunc_param_typed`
  ([tccgen.c:14642](../tccgen.c#L14642)–[14664](../tccgen.c#L14664)) applies the
  C default-argument promotions to anonymous variadic args: **`float`→`double`**
  at [tccgen.c:14661](../tccgen.c#L14661) (`gen_cast_s(VT_DOUBLE)`), bitfield
  promotion just below. This is the single biggest false-positive risk and is why
  the profile passes **only `int`** through the `...` (see §9).

Prior-art tests to mirror in shape and use as gold-reference fixtures:
[`tests/frontend/types/09_variadic.c`](../tests/frontend/types/09_variadic.c),
[`tests/frontend/pp/04_variadic.c`](../tests/frontend/pp/04_variadic.c), and the
GCC torture cases that define the failing class —
`gcc.c-torture/execute/stdarg-1.c`…`stdarg-4.c`, `strct-stdarg-1.c`, and
`va-arg-1.c`…`va-arg-*.c` under
[`tests/gcctestsuite/gcc-testsuite/gcc/testsuite/`](../tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/).

---

## 3. UB-freedom invariants (the authoring contract, restated + enforced)

1. **Default stream untouched.** Every emission below is gated on
   `g.has("varargs")`. When the flag is absent, **no `rng` value is drawn and no
   text is produced** — mirroring exactly how `float` gates `_prologue`,
   `statement`, and `generate_program` on `has("float")` (see
   [gen_c.py:353](../tests/fuzz/gen_c.py#L353),
   [:477](../tests/fuzz/gen_c.py#L477),
   [:607](../tests/fuzz/gen_c.py#L607)). Prove byte-identity:
   `diff <(gen_c.py --profile int --seed S) <(git stash; gen_c.py --profile int --seed S)`
   for a handful of seeds S after the change (M0).
2. **Arg count always matches `n` (no UB).** `vsum`'s first named parameter `n`
   is *the count of variadic args*, and the callee calls `va_arg` **exactly `n`
   times** (a `for (i = 0; i < n; i++)` loop). The generator computes the count
   once, emits that literal both as the `n` argument and as the number of trailing
   args — they cannot disagree. Reading exactly as many args as were passed is the
   sole correctness precondition of `va_arg`; satisfying it structurally makes
   every divergence a real miscompile.
3. **Single non-promotable variadic type.** *Every* trailing argument is an
   `int` (32-bit on this target), and `vsum` reads each with `va_arg(ap, int)`.
   `int` is its own default-argument-promotion result, so there is **no promotion
   ambiguity** — no `char`/`short` (would promote to `int`) and no `float` (would
   promote to `double` at [tccgen.c:14661](../tccgen.c#L14661), and reading it
   with `va_arg(ap,int)` would be UB / a guaranteed false positive). See §9.
4. **Values masked / defined.** Each trailing arg expression is an ordinary
   generator `expr()` result reduced to a value in `int` range. Because the inner
   compute is the same unsigned-modular arithmetic the `int` profile already
   guarantees defined ([gen_c.py:165](../tests/fuzz/gen_c.py#L165) docstring), no
   trailing arg can be UB. The sum inside `vsum` is accumulated in **`unsigned`**
   (defined modular wraparound), so summation never signed-overflows.
5. **Output-sensitive.** Each `vsum(...)` result is folded into `cs` via `csmix`,
   exactly like direct-helper results at
   [gen_c.py:627](../tests/fuzz/gen_c.py#L627).
6. **`va_end` always emitted.** Every `va_start` is paired with a `va_end` on all
   paths (single straight-line callee, no early return), so the ABI contract is
   complete and matches what gcc expects.
7. **Bounded & terminating.** `vsum`'s loop trips exactly `n` times with `n`
   bounded (§4, `n ∈ [0, MAX_VARARGS]`); no recursion (`vsum` calls nothing), no
   allocation. Call sites add a fixed number of arguments, no loop.

---

## 4. Emission shapes (concrete C)

**(a) The variadic helper** — emitted once in the prologue when `has("varargs")`,
right after `csmix` (so `<stdarg.h>` and `vsum` exist before `main`):

```c
#include <stdarg.h>

/* varargs profile: sum n int args; result is unsigned (defined wraparound),
 * read with va_arg(ap,int) — int is its own promotion, no ABI ambiguity. */
static unsigned vsum(unsigned n, ...)
{
  va_list ap;
  unsigned acc = 0u;
  unsigned i;
  va_start(ap, n);
  for (i = 0u; i < n; i++)
    acc += (unsigned)va_arg(ap, int);   /* exactly n reads — never over/under */
  va_end(ap);
  return acc;
}
```

`n` is `unsigned` (an already-defined display type in this generator) and the
named first parameter, so it occupies `r0`; the anonymous `int` args begin in
`r1` and spill onto the stack once `r1–r3` are full — deliberately straddling the
register/stack boundary.

**(b) Call sites with a *varying* number of args** — a new `statement` kind
(gated on `has("varargs")`) picks a random count `k ∈ [0, MAX_VARARGS]` and emits
`k` trailing `int` arguments, folding the result into `cs`:

```c
/* k == 0 : just the named arg (boundary: no anonymous args at all) */
cs = csmix(cs, vsum(0u));
/* k == 3 : all anonymous args still in registers r1..r3 */
cs = csmix(cs, vsum(3u, (int)(<expr>), (int)(<expr>), (int)(<expr>)));
/* k == 7 : r1..r3 full, remaining 4 anonymous args delivered on the stack */
cs = csmix(cs, vsum(7u, (int)(<e>), (int)(<e>), (int)(<e>), (int)(<e>),
                       (int)(<e>), (int)(<e>), (int)(<e>)));
```

The literal count (`0u`, `3u`, `7u`) is the **same** value used as `n` and as the
number of trailing expressions — they are emitted from one Python `k`, so they can
never desynchronize. Choosing `MAX_VARARGS = 8` guarantees seeds that exercise
*both* the all-in-register case (`k ≤ 3`) and the stack-overflow case (`k ≥ 4`),
which is exactly the `stdarg-2` frame seam.

**(c) Mandatory fold (output-sensitivity floor).** To guarantee the helper is
used even on a seed whose body never samples the new statement kind, emit one
deterministic call after the body (guarded by `has("varargs")`), e.g.
`cs = csmix(cs, vsum(2u, 1, (int)cs));` — this also defeats
`-Wunused-function` on `vsum`.

---

## 5. gen_c.py implementation sketch (exact edit points)

All line refs are against the current [`gen_c.py`](../tests/fuzz/gen_c.py).

**(a) Register the profile** — [PROFILES dict, gen_c.py:89](../tests/fuzz/gen_c.py#L89):
```python
PROFILES = {
    "int":     frozenset(),
    "float":   frozenset({"float"}),
    "varargs": frozenset({"varargs"}),   # adds a variadic vsum(n, ...) + call sites
}
```

**(b) Tunable** — near the other limits at
[gen_c.py:63](../tests/fuzz/gen_c.py#L63): `MAX_VARARGS = 8` (must be ≥ 5 so some
seeds straddle the r0–r3 boundary into stack args).

**(c) Prologue: include + helper** — in `_prologue`
([gen_c.py:457](../tests/fuzz/gen_c.py#L457)), mirror the `float` `extra_inc`
pattern at [gen_c.py:460](../tests/fuzz/gen_c.py#L460) and the appended-helper
pattern at [gen_c.py:477](../tests/fuzz/gen_c.py#L477). **Crucially the gate is
`"varargs" in features`**, so for the `int`/`float` profiles the prologue stays
byte-identical (no `rng` here — `_prologue` is pure text):
```python
extra_inc = ("#include <string.h>\n" if "float" in features else "") \
          + ("#include <stdarg.h>\n" if "varargs" in features else "")
...
if "varargs" in features:
    base += (
        "\n"
        "/* varargs profile: sum n int args (acc unsigned -> defined wraparound). */\n"
        "static unsigned vsum(unsigned n, ...)\n"
        "{\n"
        "  va_list ap; unsigned acc = 0u; unsigned i;\n"
        "  va_start(ap, n);\n"
        "  for (i = 0u; i < n; i++) acc += (unsigned)va_arg(ap, int);\n"
        "  va_end(ap);\n"
        "  return acc;\n"
        "}\n"
    )
```
*(Order matters: append the `string.h`/`stdarg.h` includes deterministically so a
seed run under `varargs` is reproducible; only the `varargs` branch is new.)*

**(d) Gated statement kind `"vcall"`** — in `statement`
([gen_c.py:344](../tests/fuzz/gen_c.py#L344)), extend `opts` exactly like the
`float` block at [gen_c.py:353](../tests/fuzz/gen_c.py#L353), then handle it:
```python
if self.has("varargs"):
    opts += ["vcall", "vcall"]
...
if kind == "vcall":
    k = self.rng.randint(0, MAX_VARARGS)           # one draw -> count == n == #args
    args = ", ".join(f"(int)({self.expr(MAX_EXPR_DEPTH)})" for _ in range(k))
    sep = ", " if k else ""
    return [f"{pad}cs = csmix(cs, vsum({k}u{sep}{args}));"]
```
The `expr()` draws here happen **only** inside `if self.has("varargs")`, so the
`int`/`float` streams are untouched. `(int)(...)` truncates each well-defined
unsigned `expr` to `int` range; since the callee reads it back with
`va_arg(ap,int)` the round-trip is exact.

**(e) Mandatory fold** — in `generate_program`, right after the helper-call fold
loop at [gen_c.py:626](../tests/fuzz/gen_c.py#L626)–[627](../tests/fuzz/gen_c.py#L627),
add (gated):
```python
if g.has("varargs"):
    main.append("  cs = csmix(cs, vsum(2u, 1, (int)cs));")
```
This guarantees `vsum` is referenced (no `-Wunused-function`) and the program is
output-sensitive to variadic codegen even when no `vcall` statement was sampled.

**(f) No other edits.** `vsum` calls nothing (it is a leaf in the call graph), so
it never interacts with `g.helpers` / `g.callable_helpers`; the `int`-profile
helper machinery is unaffected. `g.fresh` is not used (the helper name `vsum` is
fixed), so no counter perturbation either.

---

## 6. Oracle

**Primary: vs-gcc** ([`test_random_c_vs_gcc.py`](../tests/fuzz/test_random_c_vs_gcc.py),
gold = `arm-none-eabi-gcc -O2`). Variadic calls are **pure ABI surface**: the
frame layout, the `r0–r3` spill, the `__gr_top`/named-byte bookkeeping, and the
`va_arg` walk are all places where *every* tcc opt level can agree and still be
wrong — exactly the **O0-WRONG** blind spot olevels structurally cannot see
(master plan §0/§5). The `va-arg-*` / `stdarg-*` torture failures are precisely
this class. So vs-gcc is the **certifying gate** for this profile, and the only
oracle that catches its headline bug class.

**Secondary: olevels** ([`test_random_c_olevels.py`](../tests/fuzz/test_random_c_olevels.py))
still adds value: it certifies **opt stability** of the variadic path — e.g. an
optimizer that mis-estimates the variadic frame size and shrinks it under `-O1`
(the `stdarg-2` frame-shrink shape), or that CSEs/reorders the `r0–r3` save
stores. Those diverge across O0/O1/O2 and olevels catches them cheaply (and the
D4 `-fno-<pass>` knobs ride on olevels). Run **both**; vs-gcc gates, olevels finds
the cheaper opt-ordering bugs.

---

## 7. Sweep & certify (exact commands)

```bash
# Exhaustive, authoritative O-level triage (real bisect + repro per flagged seed):
FUZZ_PROFILE=varargs tests/fuzz/triage_olevels.sh 0 999
#   -> writes fuzz_triage_varargs_0_999.md
#   (plumbing: triage_olevels.sh:30 keys off FUZZ_PROFILE; ~80%+ recall is the
#    pre-scan note for batch_sweep, NOT this exhaustive script)

# Fast pre-scan only (~200 seeds/boot, ~80% recall -- NEVER certify with this, only find):
python3 tests/fuzz/batch_sweep.py 0 5000 --profile varargs
#   (batch_sweep.py:139 / :332 honor FUZZ_PROFILE / --profile)

# Both pytest wrappers under the varargs profile:
FUZZ_PROFILE=varargs pytest -s tests/fuzz/test_random_c_olevels.py
FUZZ_PROFILE=varargs FUZZ_VSGCC_SEEDS=0-999 pytest -s tests/fuzz/test_random_c_vs_gcc.py

# Inspect one generated program:
python3 tests/fuzz/gen_c.py --profile varargs --seed 42
```

**Certify:** exhaustive `0–N` **green on vs-gcc** (the ABI gate) *and* clean
olevels, growing the certified band per master-plan §4 (`batch_sweep` only to
*find* candidates, `triage_olevels.sh` to certify). Promote vs-gcc to a wider
swept band (`FUZZ_VSGCC_SEEDS=0-9999`) once `0–999` is green.

---

## 8. Harvest

Per confirmed root cause, add a regression test continuing the sequence — the
**highest existing is `219_*`** (verified: `ls tests/ir_tests | grep -oE '^[0-9]+'
| sort -n | tail` → 219, e.g.
[`219_fuzz_strd_spill_dryrun_offset.c`](../tests/ir_tests/219_fuzz_strd_spill_dryrun_offset.c)).
Numbers are assigned **at harvest time** (start `220_fuzz_<cause>.c`), each with
its `.expect` (single line `checksum=<hex>`, the gcc-reference value) and
registered in `TEST_FILES` in
[`tests/ir_tests/test_qemu.py`](../tests/ir_tests/test_qemu.py) (the list around
[test_qemu.py:433](../tests/ir_tests/test_qemu.py#L433)):

```
tests/ir_tests/220_fuzz_<cause>.c        # minimized repro from gen_c.py --profile varargs --seed S
tests/ir_tests/220_fuzz_<cause>.expect   # gold output (gcc reference; or tcc -O0 only if O0 confirmed correct)
```

**Fuzz memory = the test's own comment header.** In this repo the "fuzz memory per
root cause" is the structured comment block at the top of each
`NN_fuzz_<cause>.c` (as on
[`218_fuzz_loop_unroll_branch_fallthrough.c`](../tests/ir_tests/218_fuzz_loop_unroll_branch_fallthrough.c)):
record `(profile=varargs, seed=S)`, the pass/file the bug lives in (e.g. the
prologue spill in [arm-thumb-gen.c:9676](../arm-thumb-gen.c#L9676) or the runtime
in [lib/va_list.c:104](../lib/va_list.c#L104)), the mechanics, the fix, the
divergent O-levels, and the correct `gcc -m32 -funsigned-char` / vs-gcc checksum.
Root-cause with [`debugging_fuzz_divergences.md`](debugging_fuzz_divergences.md) /
`scripts/bisect_opt.py`; cadence per
[`fuzz_triage_guide.md`](fuzz_triage_guide.md).

---

## 9. Feature-specific false-positive traps (and how the generator avoids each)

| Trap | Why it would be a false positive | Generator defense |
|---|---|---|
| **Default argument promotion mismatch (`char`/`short`)** | A `char`/`short` variadic arg is promoted to `int` by both compilers; if the callee read it with `va_arg(ap,char)` that is UB and the two could legally disagree. | The `...` carries **only `int`**; the callee reads with `va_arg(ap,int)`. `int` is its own promotion result, so caller-promoted type == callee-read type by construction. (§3.3) |
| **`float`→`double` promotion** | A `float` variadic arg is promoted to `double` at [tccgen.c:14661](../tccgen.c#L14661) (`gen_cast_s(VT_DOUBLE)`); reading it back as `va_arg(ap,int)` (or even `va_arg(ap,float)`) is UB → guaranteed false positive. | **No FP ever flows through `...`.** Trailing args are `(int)(...)` casts of integer `expr`s. The `varargs` profile does not enable the `float` flag, so `g.fvars` is empty and no FP value can reach a `vcall`. |
| **Arg-count mismatch (under/over-read)** | Calling `va_arg` more or fewer times than args passed is UB; conforming compilers may differ. | The count `k` is drawn **once** and used both as the `n` argument and as the number of trailing expressions; `vsum` reads exactly `n` times in a `for` loop. They cannot desynchronize. (§3.2) |
| **`va_arg` type ≠ passed type** | Reading a different type than was passed is UB even with matching sizes. | Caller passes `int`, callee reads `int`. Single fixed type, no per-call type variation. |
| **Missing `va_end` / unbalanced `va_start`** | Some ABIs require `va_end`; omitting it is UB on those. | `vsum` always pairs `va_start`/`va_end` on its single straight-line path. (§3.6) |
| **Signed overflow in the sum** | Accumulating `int` args could signed-overflow → UB. | `acc` is `unsigned` (defined modular wraparound); the cast-back to the display type happens only in `cs`, which is unsigned. |
| **Reading the named arg via `va_arg`** | `va_arg` must start *after* the last named param; reading `n` itself is UB. | `va_start(ap, n)` passes the correct last-named parameter; the loop reads only the anonymous args. |

---

## 10. Milestones (ordered checklist)

- [x] **M0 — byte-identity guard.** Before any edit, snapshot
      `gen_c.py --profile int --seed {0,1,2,7,42,100}` (and the same for
      `--profile float`). After every change, re-diff: must be byte-identical.
      (Same proof the `float` profile passed.)
- [x] **M1 — register profile.** Add `"varargs"` to `PROFILES`
      ([gen_c.py:89](../tests/fuzz/gen_c.py#L89)) and `MAX_VARARGS`. Verify
      `--profile varargs` runs and the `int`/`float` diffs are clean (M0).
- [x] **M2 — prologue helper.** Emit `#include <stdarg.h>` + `vsum` in
      `_prologue` ([gen_c.py:457](../tests/fuzz/gen_c.py#L457)), gated on
      `"varargs" in features`. Inspect `--profile varargs --seed N`: helper
      present once, `<stdarg.h>` included, prologue otherwise unchanged.
- [x] **M3 — gated call site.** Add the `"vcall"` statement kind
      ([gen_c.py:344](../tests/fuzz/gen_c.py#L344)) and the mandatory post-body
      fold ([gen_c.py:626](../tests/fuzz/gen_c.py#L626)). Confirm: count == #args
      always; some seeds emit `k ≥ 5` (stack args) and some `k == 0`; result flows
      into `cs`; no FP in any `...`.
- [x] **M4 — compile both oracles.** `--profile varargs --seed {0..20}` compiles
      clean under `armv8m-tcc` **and** `arm-none-eabi-gcc -O2` with no new
      warnings (esp. `-Wunused-function`, no `-Wvarargs`/promotion warnings); the
      two stdouts agree on a non-buggy seed.
- [x] **M5 — sweep.** `FUZZ_PROFILE=varargs tests/fuzz/triage_olevels.sh 0 999`
      and `batch_sweep.py 0 5000 --profile varargs`; collect candidate seeds.
- [x] **M6 — vs-gcc gate.** `FUZZ_PROFILE=varargs FUZZ_VSGCC_SEEDS=0-999 pytest …
      test_random_c_vs_gcc.py`; triage every divergence with `bisect_opt.py`
      (expect hits in the prologue spill / `__tcc_va_arg` walk — the `va-arg-*`
      class).
- [ ] **M7 — harvest.** Minimize each root cause → `ir_tests/220_fuzz_*.c`
      (+`.expect`, register in `test_qemu.py`); write the fuzz-memory comment
      header per cause; fix; re-certify the band green on both oracles.
- [ ] **M8 — certify & advance.** Exhaustive `0–N` green (vs-gcc + olevels);
      record the certified band; advance the frontier per master-plan §4.
