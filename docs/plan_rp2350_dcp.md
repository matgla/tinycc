# Plan: RP2350 floating point via the DCP coprocessor

Companion to [`plan_vfp_hard_float.md`](plan_vfp_hard_float.md); phases 4 and 5
below depend on the codegen path that plan describes.

## Context

RP2350's Cortex-M33 has two independent FP resources:

- an **FPv5-SP FPU** (CP10/CP11) for single precision, and
- the **DCP**, Raspberry Pi's own double coprocessor on **CP4**.

The DCP is not an FPU. It supplies primitives that a short instruction sequence
composes into an IEEE double operation, and it operates on **GPR pairs** via
`mcrr`/`mrrc` rather than on any FP register file. That last point is what makes
this tractable: doubles keep the ordinary soft-float ABI and the existing
register allocator, so there is no double register class to add.

### How arm-none-eabi-gcc solves it, and where the gap is

GCC has no DCP knowledge and no `-mfpu` for it. pico-sdk ships hand-written
assembly (`double_aeabi_dcp.S`) and redirects the compiler's calls with
`-Wl,--wrap=__aeabi_dadd`. Every double operation is therefore **a function
call**, and each wrapper pays `push {lr}` + an engaged-check + `bx lr` on top of
the sequence, plus r0–r3 marshalling at the call site.

That call overhead is the gap TCC can close. `__aeabi_dadd` is 6 DCP
instructions wrapped in ~8 instructions of ABI and reentrancy scaffolding;
emitted inline it is 6 instructions on registers the allocator already chose. A
double compare is better still: `RCMP` writes NZCV directly, so `a < b` becomes
4 instructions and a conditional branch instead of a call plus flag decode.

**The sequences do not have to be invented.** The full pico-sdk is vendored at
`tests/benchmarks/libs/pico-sdk/`, including
`src/rp2_common/hardware_dcp/include/hardware/dcp_instr.inc.S` (mnemonic →
encoding) and `dcp_canned.inc.S` (the sequences). `tools/copro_dis.py` lists raw
32-bit encodings, which the encoder unit tests are checked against.

### Two user-selectable modes

| Mode | Flag | `float` | `double` | Binary |
|---|---|---|---|---|
| **Portable** | default | `BL __aeabi_f*` | `BL __aeabi_d*` | Runs anywhere; the linked `.a`, or the yaff `.so` resolved at load time, decides the implementation |
| **Fast** | `-mfpu=rp2350` | inline `vadd.f32` … (arithmetic; compare/convert still call) | inline DCP add/sub/compare (mul/div/convert still call) | RP2350-only |

Portable mode is not slow on RP2350 either, because `librp2350fp` is itself
DCP-backed — it pays call overhead, not a soft-float algorithm.

**ABI decision: `-mfpu=rp2350` implies softfp, not hard-float.** Inside a
function floats live in `s0-s15` and doubles are computed via DCP; at function
boundaries both stay in GPRs. That keeps fast-mode objects link-compatible with
portable-mode objects and `.so` plugins, and skips phases 3–4 of the VFP plan
(AAPCS `s0-s15`/`d0-d7` placement) entirely. For doubles, hard-float ABI is
actively *worse* here: it would pass them in `d0`/`d1` and every DCP sequence
would need `vmov` round-trips — exactly the `#if defined(__ARM_PCS_VFP)` blocks
in pico-sdk's `double_aeabi_dcp.S`. **Keep doubles on the GPR ABI permanently.**

---

## Status

### Landed and verified

**Phase 1 — coprocessor instructions.** Nothing in-tree could emit or assemble a
CP4 instruction; `thumb.h`'s `coproc` feature bit was reserved but never set.

- `source/backend/arch/arm/thumb/thop_coproc.{c,h}` — `cdp/cdp2`, `mcr/mcr2`,
  `mrc/mrc2`, `mcrr/mcrr2`, `mrrc/mrrc2`.
- `MRC` with `Rt == PC` handled as **`apsr_nzcv`** — load-bearing, since that is
  what lets an inline double compare feed a plain conditional branch. Side
  effect: `vmrs apsr_nzcv, fpscr` now parses too; it previously did not.
- `.coproc = 1` on all four Mainline profiles (matches GAS, which needs no extra
  flag for `mcr` on cortex-m33).
- Tokens `p0`–`p15`, `c0`–`c15`, `cr0`–`cr15` + parser + dispatch.

Verified: `.text` **byte-identical to `arm-none-eabi-as`** over 14 instructions;
35 unit tests (`tests/unit/arm/armv8m/test_thop_coproc.c`); 40+ instruction
assembler test (`tests/thumb/armv8m/test_coproc.S`). Expected encodings were
*generated* by `arm-none-eabi-as` and cross-checked against `copro_dis.py`, not
hand-computed. Mutation-tested: perturbing the encoder's `opc1` shift fails the
test.

**FP conformance harness.** `tests/fp/` — a host generator producing 3,057
double + 2,803 float bit-exact vectors, a shared runner, and three ways to run
it: host (`run_host_softfp_test.sh`, 0.3 s), QEMU
(`tests/ir_tests/421_fp_conformance.c`), and RP2350 hardware
(`tests/benchmarks/run_fp_conformance.py`). Expected values come from the host,
not from a TCC-vs-GCC diff, so a bug present in both compilers still fails.

**libsoftfp correctness — 524 → 0 failures.** The harness found two systematic
bugs on its first hardware run: no round-to-nearest (truncation toward zero) and
broken denormal handling. Fixed with a shared rounding core in
`lib/fp/soft/soft_common.h` (`sfp_shr_sticky32/64`, `sfp_round_pack_float/double`)
used by all eight files, plus three site-specific bugs (`dmul`'s power-of-two
fast path flushed subnormals; `fmul` narrowed the product with a fixed shift
valid only for full-width products; `f2d_bits` treated a subnormal float's
significand as if it had an implicit leading 1).

**Link shadowing.** `-lsoftfp` resolved to `lib/fp/libsoftfp.so` (yaff) before
`libsoftfp.a`, leaving libgcc to supply every `__aeabi_*` — which is why those
524 bugs sat invisible in a 2,500-test suite. `tests/ir_tests/qemu/mps2-an505/Makefile`
now names the archive explicitly.

**Phase 2 — `-mfpu=rp2350` plumbing.** Enum, option parsing (`rp2350` and
`rp2350-dcp`), feature resolution, `arm_rp2350_dcp_fpu_config`
(`source/backend/arch/fpu/arm/rp2350-dcp.c`), and `tccelf_get_fp_lib_name` →
`"rp2350fp"`.

Also fixed a latent bug found here: **`fpv5-sp-d16.c` and `fpv5-d16.c` were never
compiled.** Their headers declared `const FloatingPointConfig x;` with no
`extern` — a tentative definition — so `arm-thumb-gen.c` silently supplied a
zero-initialised copy and both capability tables were dead code. Headers are now
`extern`, and `source/backend/arch/arm/Makefile` builds them.

**Phase 3a — DCP sequences.** `lib/fp/arm/rp2350/dcp_aeabi.S` implements
dadd/dsub/drsub/dmul, all six comparisons plus the flag-setting forms,
i2d/ui2d/d2iz/d2uiz/f2d/d2f via DCP, and the float ops via FPv5-SP.
`dcp_init.c` replaces the fictional `0x50200000` MMIO block with the real CPACR
CP4/CP10/CP11 enable. Assembles clean; `__aeabi_dadd` produces exactly the six
verified encodings.

**Phase 3b — `librp2350fp` is self-contained.** `-mfpu=rp2350` links.

- `lib/fp/soft/conv64.c` — `f2lz`/`f2ulz`/`d2lz`/`d2ulz` split out of `conv.c`
  and `dconv.c`, so a hardware runtime can take the 64-bit conversions (neither
  the DCP nor FPv5-SP has a 64-bit integer port) without also taking the 32-bit
  ones it implements itself.
- `lib/fp/arm/rp2350/Makefile` compiles `ddiv.c`, `fcmp.c`, `fcmp_asm.S`,
  `conv64.c` from `../../soft` into the archive. No object is shared with
  `conv.o`/`dconv.o`/`dcmp*.o`, so nothing collides with the DCP versions.
- `dcp_aeabi.S` gained `cdcmplt`/`cdcmpgt`/`cdcmpge` (aliases of `cdcmple`,
  exactly as in libsoftfp), `__aeabi_dneg`, and `__aeabi_f2d_bits`.
- **The flag-setting compares were rewritten** — see the ABI note below. This
  was a real miscompare, not a cleanup.
- `make check-self-contained` asserts `nm librp2350fp.a ⊇ nm libsoftfp.a` over
  `__aeabi_*` and runs on every `build`. Mutation-tested: dropping `ddiv.c`
  from the source list fails the build naming `__aeabi_ddiv`.

Verified: the pico-sdk conformance image links and `__aeabi_dadd` in the final
ELF is the six-instruction DCP sequence, with `ddiv`/`d2lz`/`dneg`/`f2d_bits`
all resolved from `librp2350fp.a` and no libgcc `ieee754-df.o`.

#### `cdcmple` speaks tcc's flag convention, not the AEABI's

The AEABI encodes a three-way compare in ZCV for *unsigned* conditions (`cc` for
`<`, `ls` for `<=`), and pico-sdk's `double_aeabi_dcp.S` is conformant because
that is what GCC tests. **tcc is not.** `ir/gen/float.c` swaps the operands for
`>`/`>=` and the backend then tests *signed* conditions:

```
bl __aeabi_cdcmple ; ite lt ; movlt r0,#1 ; movge r0,#0
```

which only works against libsoftfp's convention — flags as if `cmp r,#0` on a
`-1/0/1/2` result (`fcmp_asm.S`, `dcmp_asm.S`). The transcribed-from-pico
version read RCMP straight into `apsr_nzcv`, so every `<`/`<=`/`>`/`>=` on a
double would have tested N against a bit RCMP uses for "engaged". `.Ldcmp_flags`
now decodes RCMP's Z/C/V into that three-way form. The float side needed nothing
— it uses soft `fcmp_asm.S`, which is already in the same convention.

Keeping the two libraries in one convention is what makes the plan's
portable-mode/fast-mode link compatibility true: the same object emits the same
call and the same condition code either way. It also means neither library is
callable from GCC-compiled code; see *Later*.

**Unit-test build.** `tests/unit/arm/armv8m/Makefile` did not link the
`arch/fpu/arm/` tables, so `run_unit_tests_backend` failed to link once their
headers became `extern`. Fixed.

**Literal-pool overflow — a silent wrong-code bug in the backend, found here.**
Growing `fp_conformance.c` by the per-op histogram made it HardFault at `-O2`
under QEMU. The cause was not in the harness:

`th_literal_pool_generate()`'s second pass masked the computed offset into the
instruction (`|= ((aligned_position - 4) >> 2) & 0xff`) with **no range check**,
at all three sites (LDR T1, LDRD, LDR.W). Every flush trigger tested
`code_size + pool_count * 4 >= 1020`, which undercounts the real load-to-pool
distance by up to 3 bytes (the test runs after `code_size += op.size`), 2 bytes
(pool alignment padding) and 16 bytes (a flush suppressed by `op_in_it_block`).
The T1 encodable ceiling is 1024. At 1036 the offset wrapped mod 1024 and the
load pulled its "constant" out of the middle of the instruction stream — no
diagnostic, a plausible binary, a fault only when that path ran.

Fixed in two parts: `THUMB_POOL_FLUSH_BUDGET` (1000) now names the threshold at
every site that models it, leaving 24 bytes of headroom; and the patch loop
`tcc_error`s instead of masking, so a future drift is a build failure rather
than a miscompile. Cost measured over the 1,545-file gcc-torture execute corpus
at `-O2`: **+34 bytes total** (0.003%).

Worth knowing: this was reachable by any sufficiently large function with an
early literal load, not just FP code.

**Phase 3c — validated on hardware. The DCP path is correct over the whole
normal range; it has no subnormals.**

```
RP2350 (tcc -O1, rp2350fp, -mfpu=rp2350)
  double 2898/3057   float 2803/2803
  VERDICT: PASS with FTZ (159 subnormal-flush deviations, 0 other)
```

All 159 failures have a subnormal operand or result — the per-op histogram
shows `failed` equal to `subnormal` in every row:

```
d add    31/ 452 failed (31 subnormal)     d div    452/452 ok
d sub    30/ 452 failed (30 subnormal)     d neg    ... ok
d mul    66/ 452 failed (66 subnormal)     d i2f/u2f/l2f/ul2f ... ok
d cmp    28/1024 failed (28 subnormal)     d f2i/f2u/f2l/f2ul ... ok
d widen   4/  30 failed ( 4 subnormal)     d narrow  32/ 32 ok
                                           f (all 2803) ok
```

The DCP flushes subnormals to zero and offers no path that doesn't;
pico-sdk's `double_aeabi_dcp.S` doesn't handle them either — its own comments
read *"very small; we don't care that we might make a denormal"*. So this is a
property of the coprocessor, not of this port. Single precision is unaffected
because it runs on the FPv5-SP FPU, which is fully IEEE.

`run_fp_conformance.py --allow-ftz` accepts exactly that deviation and nothing
else, so the gate stays meaningful: any non-subnormal vector turns it red.

**The comparison risk flagged for this phase did not materialise** — 996 of
1024 compare vectors pass, and the 28 that fail are subnormal operands whose
flush-to-zero changes the relation. The RCMP Z/C/V reading inferred in Phase 3b
(`bit30=Z` equal, `bit29=C` ordered-`≥`, `bit28=V` unordered) is confirmed
correct over every NaN/Inf/zero pairing.

**Library-mode measurement — 24.5x on add, 25.7x on a mixed kernel.**

`tests/benchmarks/bench_double.c` (new; the suite had *no* `double` in it, and
`bench_math.c`'s "float_math" is an integer loop returning a constant, so any
earlier softfp-vs-DCP comparison would have measured nothing). Total cycles,
TCC `-O1`, RP2350:

| kernel | softfp | rp2350fp | speedup |
|---|---:|---:|---|
| `double_add` — dadd/dsub chain | 4,461,958 | 182,100 | **24.5x** |
| `double_mixed` — Mandelbrot | 6,330,114 | 246,437 | **25.7x** |
| `double_cmp` — comparisons | 2,231,467 | 205,010 | **10.9x** |
| `double_mul` — dmul chain | 6,451,709 | 2,360,015 | **2.7x** |
| `double_div` — *control* | 10,186,277 | 8,973,859 | 1.14x |

Code size: **88,328 → 83,768 bytes (−4,560, −5.2%)**.

`double_div` is the control and behaves like one: `__aeabi_ddiv` is
`lib/fp/soft/ddiv.c` in *both* builds, so its 1.14x is only the i2d/dmul/dadd/
compare around the divisions getting faster. That it moved least by an order of
magnitude is what says the other numbers are the DCP and not clock or link
drift.

`dmul` gains least of the real sequences (2.7x) because it is 13 instructions
with 7 scratch registers and a `push {r4, lr}`, against 6 register-only
instructions for `dadd`.

Every kernel reports **PASS** in both builds against a single shared expected
value, so the DCP is bit-identical to the bit-exact soft implementation on all
five — a correctness cross-check at speed, not just a timing harness.

**This is all library-side.** `-mfpu=rp2350` is still inert for codegen (the
`has_*` bits in `rp2350-dcp.c` are all clear): compiling `bench_double.c` with
and without it produces byte-identical objects. Phase 4's inlining is headroom
*on top* of these numbers — it removes the remaining `bl` + ABI marshalling,
which at 182,100 cycles for 6,000 `dadd`s (~30 cycles each, for a 6-instruction
sequence) is now most of what is left.

**Phase 5 (partial) — inline VFP single-precision landed.**
`thumb_emit_vfp_arith_mop()` emits `vmov s0,rn ; vmov s1,rm ; v<op>.f32 s0,s0,s1
; vmov rd,s0` for fadd/fsub/fmul/fdiv — four instructions against a BL plus the
library's own five.

s0/s1 are *fixed scratch*, not allocated: this is the softfp ABI, so no float
value ever lives in a VFP register between operations and s0-s15 are free at
every point.  That is what makes this independent of Phase 5 proper — making
floats *live* in s0-s15 needs a register class and `th_vldr`/`th_vstr`, which
still do not exist.  Values move through GPRs on both sides, so nothing here
depends on the float ABI and fast-mode objects stay link-compatible.

Hardware: **float 2803/2803**, every add/sub/mul/div vector — and unlike the
DCP, FPv5-SP is fully IEEE, so there are no subnormal deviations on the float
side at all.

`-mfloat-abi=soft` had to be excluded explicitly.  `soft` means "emit no FPU
instructions"; `softfp` (the default, and what `-mfpu=rp2350` keeps) means FPU
instructions with GPR argument passing.  The `has_f*` bits describe silicon,
not ABI, so without a separate check `-mfloat-abi=soft` started emitting
`vadd.f32` — caught by `test_fp_soft_float_uses_runtime_helpers`.

`test_fp_hard_float_uses_vfp` was a `strict=True` xfail written to fire exactly
when this landed; it now asserts the VFP instructions positively.  It checks
only the *single*-precision ops: on fpv5-sp-d16 there is no double unit, so
`addd` legitimately stays a call.

**A latent miscompile found here.** `fpv5-d16.c` claimed **every** operation was
inline (`has_fadd` … `has_dneg` all 1) and `fpv5-sp-d16.c` claimed most of them,
while the backend implemented none.  With a bit set, the IR stops rewriting the
op into a call, the backend emits a `BL` anyway, and
`ir_op_is_implicit_call_ra()` — reading the same bits — stops modelling the
r0-r3 clobber.  That is wrong code, not a missed optimisation.  It was inert
only while those headers lacked `extern` and every includer got a zero-filled
copy; Phase 2 fixing that made the claims live.  Both tables now assert only
what has an emitter.

**Phase 4 (partial) — inline DCP `dadd`/`dsub`/`dcmp` landed.** The first
inline FP emission in this backend.

- `FpDoubleImpl` / `.double_impl` in `tcc_target.h` — the `has_d*` bits say
  *whether* an op is inline, this says *what to emit*, since "double add is
  inline" means `vadd.f64` on fpv5-d16 and a CP4 sequence on RP2350.
- `thumb_emit_dcp_addsub_mop()` (`arm-thumb-gen.c`) — `WXUP;WYUP;ADD0;ADD1|SUB1;
  NRDD;RDDA|RDDS`, six instructions, no scratch, on the registers the allocator
  already chose. `mcrr`/`mrrc` move a GPR pair but place no consecutiveness
  constraint on it (that is LDRD's rule), so nothing is shuffled into r0-r3.
- `thumb_emit_dcp_cmp_mop()` — `WXUP;WYUP;ADD0;RCMP apsr_nzcv`, four
  instructions and no result register.  The RCMP is emitted *after*
  `mach_release_all()` so the flags are the last thing written before the
  branch reads them; splitting it from ADD0 is safe because the DCP holds its
  state across unrelated core instructions, which is the same property
  `__rp2350_dcp_save`/`_restore` rely on.
- `ir/gen/float.c` learned the DCP compare convention.  RCMP's flags are the
  AEABI encoding read with *unsigned* conditions, not libsoftfp's signed
  three-way, so the fix-up is a different one: `>`/`>=` map straight onto
  `HI`/`HS` and are **not** swapped, while `<`/`<=` mirror the operands
  (RCMP has no "less than" condition) and also test `HI`/`HS`.  Unordered
  falls out correctly everywhere because RCMP leaves C clear on unordered.
- `.has_dadd`/`.has_dsub`/`.has_dcmp` set in `rp2350-dcp.c`.

Hardware, versus the library mode measured above:

| kernel | softfp | library | + add/sub | + cmp | vs softfp |
|---|---:|---:|---:|---:|---|
| `double_add` | 4,461,958 | 182,100 | 155,101 | 155,103 | **28.8x** |
| `double_mixed` | 6,330,114 | 246,437 | 235,615 | **219,522** | **28.8x** |
| `double_cmp` | 2,231,467 | 205,010 | 198,013 | **103,079** | **21.6x** |
| `double_mul` | 6,451,709 | 2,360,015 | 2,353,016 | 2,340,018 | 2.8x |
| `double_div` | 10,186,277 | 8,973,859 | 8,966,860 | 8,953,862 | 1.14x |

The compare is the single biggest step: **−48%** on `double_cmp`, because
inlining it removes the call *and* the flag decode at once — RCMP writes NZCV
and the branch consumes it directly. `mul` and `div` moving under 1% across
both steps is the internal control: they are still library calls, so only the
add/sub/compare kernels should have changed, and only they did.

Code size 83,768 → 84,152 (+384): a few instructions per site instead of one
shared copy, the expected trade.

Conformance is unchanged at `add 31/452, sub 30/452, cmp 28/1024` — the same
subnormal-only failures as the library, so each inline sequence is
bit-identical to the verified library one.  For the compare that is a strong
result rather than a formality: the vectors cover all six relations over every
NaN/Inf/zero pairing, so the unsigned-condition mapping is confirmed end to
end.

**The `has_*` bits are a shared contract, not a hint.** `ir_op_is_implicit_call_ra()`
(`ir/regalloc.c`) reads the same bits to decide whether an operation still
clobbers r0-r3 like a call. Setting a bit while the backend keeps emitting a
`BL` does not merely miss an optimisation — it un-models a real clobber. Flip a
bit and land its emitter in the same commit.

Note the RA win is *not* collected yet: that gate is
`!(fpu->has_fadd && fpu->has_dadd)`, so it only stops treating FADD as a call
once the **float** side is inline too (Phase 5). Until then the allocator stays
conservative, which costs registers but is safe.

### Current numbers

```
host   (lib/fp/soft, native)          0 / 5860 failures
QEMU   (mps2-an505, libsoftfp)        0 / 5860 failures
RP2350 (tcc -O1, libsoftfp)           3057/3057 double, 2803/2803 float  PASS
RP2350 (tcc -O1, rp2350fp, DCP)       2898/3057 double, 2803/2803 float  PASS with FTZ
QEMU IR suite 2552 passed | unit 2720 + 257 passed | asm 162 passed
gcc-torture IR 13857 passed
```

Pre-existing and unrelated: `make test-frontend` has 9 failures that are pure
`GlobalSym(1185)` → `GlobalSym(1243)` drift. The goldens were last regenerated
at `9c1a2251`; the coprocessor token block landed later, in `6ff88972`. They
need a reviewed `--update`, not a code fix.

---

## Next steps

Roughly in the order they are worth doing:

| # | Item | Why now | Size |
|---|---|---|---|
| 1 | **Ratify the FTZ policy** (3c-bis) | A user-visible correctness contract that is currently undocumented; blocks recommending the flag | decision + docs |
| 2 | **Reentrancy** (3d) | The preemptor-saves contract is written down but not implemented in either half; invisible to every test we have | small–medium |
| 3 | **`dneg` + DCP conversions** (4) | Zero-scratch, sequences already hardware-verified in `dcp_aeabi.S`; transcription plus a bit each | small |
| 4 | **VFP `fneg` / conversions / `fcmp`** (5) | Same shape as the landed float arithmetic; `fcmp` needs a third condition mapping | small–medium |
| 5 | **`dmul`** (4) | `double_mul` is 2.8x where add/sub reach 28.8x, but 7 scratch registers need a real answer | medium |
| 6 | **Teach the optimizer the inline path** (Later) | Folding now silently stops applying wherever an op stopped being a recognisable `__aeabi_` call | medium |
| 7 | **Floats live in `s0-s15`** (5 proper) | The remaining `vmov` pair per op; needs `th_vldr`/`th_vstr` and a register class | large |

Items 3 and 4 are the cheap ones. Item 6 is the easiest to forget and the
easiest to regress on, because nothing fails — the code just gets bigger.

### Phase 3c-bis — decide the subnormal policy *(immediate)*

The measurement is done; the policy decision is not.

**1. Ratify FTZ (a project decision, not a bug to fix).** `-mfpu=rp2350` is not
IEEE-754 for subnormal doubles and cannot cheaply be made so — a conforming
`dadd` would need an operand test plus a soft fallback on every call, which is
most of the win. The options:

- *accept it*, and say so where users meet the flag (`-mfpu=rp2350` help text,
  README) — this is what pico-sdk does;
- *guard it*, e.g. only allow `-mfpu=rp2350` together with an explicit
  `-ffast-math`-ish opt-in;
- *fix it in the library*, paying a subnormal check per operation, and keep the
  inline Phase-4 sequences unguarded so `-mfpu=rp2350` is fast but non-IEEE
  while `librp2350fp` alone is conforming.

Nothing else in the plan depends on which is chosen, but the answer belongs in
the docs before this ships.

**2. ~~Measure.~~** Done — see the table above. 24.5x on `dadd`, 25.7x on a
mixed kernel, −5.2% code size, all library-side. Phase 4 is worth doing.

Re-running the gate and the measurement:

```bash
tests/benchmarks/run_benchmark.py 192.168.0.113 --only tcc                       # softfp
tests/benchmarks/run_benchmark.py 192.168.0.113 --only tcc \
    --fp-lib rp2350fp --mfpu rp2350                                              # DCP
```

```bash
tests/benchmarks/run_fp_conformance.py 192.168.0.113 --fp-lib rp2350fp --mfpu rp2350 --allow-ftz
```

Everything up to the flash step is reproducible without a board:
`build_image("TCC", "1", "rp2350fp", "rp2350", True)` links a complete image
locally. `tests/ir_tests/170_nan_comparison.c` does *not* guard the DCP compare
path — it runs on QEMU, which never executes a DCP instruction.

### Phase 3d — reentrancy *(known gap, now the largest one)*

A DCP sequence is not atomic. If an interrupt preempts one and the handler also
uses the DCP, the interrupted computation is corrupted. The agreed contract is
**preemptor saves**: inlined sequences run unguarded, and any ISR touching
doubles must reach the DCP through the `__aeabi_*` entry points, which do the
engaged-check and save/restore.

`__rp2350_dcp_save` / `_restore` / `_engaged` exist in `dcp_aeabi.S`, but **the
entry points do not yet use them**, so the library is not currently ISR-safe.
The single-threaded case is correct.

**This moved up the list when Phase 4 landed.** The contract only holds if the
unguarded half is the *inlined* code and the guarded half is the library — and
until the entry points actually call save/restore, neither half is guarded.
Before `-mfpu=rp2350` is recommended for anything with interrupts, this needs
closing. Nothing in the conformance suite or the benchmarks touches it; both are
single-threaded, which is exactly why it has stayed invisible through every
green run so far.

pico-sdk's `saving_func` avoids duplicating each sequence with an `lr`-hooking
trick: the entry point sits after a `push {lr}` / `bl generic_save_state` /
`b 1f` preamble, and `generic_save_state` does `blx lr` to re-enter the body so
the body's `bx lr` lands on the restore path. It depends on exact instruction
sizes and `.p2align 2`. Port it, or use a simpler guard-enter/guard-leave pair
at ~6 instructions of overhead.

Note this is **not covered by the conformance suite** — it needs a dedicated
test with a timer ISR doing double arithmetic.

### Phase 4 — inline DCP codegen for `double` *(partially landed)*

`dadd`, `dsub` and `dcmp` are in (see Status). No allocator work was needed:
`mcrr`/`mrrc` move GPR pairs and doubles already live in GPR pairs
(`LS_REG_TYPE_DOUBLE_SOFT`), and those instructions place no consecutiveness
constraint on the pair — that is LDRD's rule, not theirs.

| Op | Sequence | Instrs | Scratch | State |
|---|---|---|---|---|
| `dadd` | `WXUP;WYUP;ADD0;ADD1;NRDD;RDDA` | 6 | 0 | **LANDED** |
| `dsub` | `WXUP;WYUP;ADD0;SUB1;NRDD;RDDS` | 6 | 0 | **LANDED** |
| `dcmp` | `WXUP;WYUP;ADD0;RCMP apsr_nzcv` | 4 | 0 | **LANDED** |
| `dneg` | `eor` high word with `0x80000000` | 1 | 0 | todo — no DCP needed |
| `d2f` / `f2d` | `WXUP;NRDF;RDFG` / `WXYU;NRDD;RDDG` | 3 | 0 | todo |
| `i2d`/`ui2d`/`d2iz`/`d2uiz` | `WX?C;ADD0;{ADD1,SUB1};N*;RD*` | 5 | 0 | todo |
| `dmul` | `WXUP;WYUP;RXMS;RYMS;umull;…;RDDM` | 13 | 7 | todo — `-O2`+ only |
| `ddiv`, `sqrt` | — | ~35 | 5 | never — always call |

**`dneg` and the conversions are the cheap ones left.** All are zero-scratch and
follow `thumb_emit_dcp_addsub_mop()` directly; the sequences are already written
and hardware-verified in `lib/fp/arm/rp2350/dcp_aeabi.S`, so this is
transcription plus a `has_*` bit each. `dneg` does not touch the coprocessor at
all — it is one `eor` on the high word, and today it still goes through the
generic FNEG path that forces the operand into r0:r1.

**`dmul` is the one with real design content.** Seven scratch registers on top of
four operands is more than `mach_alloc_scratch` will hand out under pressure, so
it needs either a spill-aware variant or an `-O2`-only gate with a fallback to
the call when the allocator cannot supply them. Worth doing — `double_mul` is
still 2.8x where add/sub reach 28.8x — but it is not a transcription job.

**Do not forget the paired edit.** Each `has_*` bit is read by *both*
`ir_put_soft_call_fpu_if_needed()` and `ir_op_is_implicit_call_ra()`; setting one
without its emitter un-models the r0-r3 clobber of a call that still happens.
Bit and emitter in the same commit, always.

### Phase 5 — inline VFP codegen for `float` *(partially landed)*

`fadd`/`fsub`/`fmul`/`fdiv` are in via `thumb_emit_vfp_arith_mop()`, using s0/s1
as fixed scratch. That deliberately sidesteps the hard part: under softfp no
float ever lives in a VFP register between operations, so no register class and
no spill/reload are needed.

Remaining, cheapest first:

- **`fneg`** — `vneg.f32`, or leave it as the existing `eor` (already inline and
  arguably better, since it avoids two `vmov`s). Decide and record which.
- **`itof`/`ftoi`/`ftof`** — `vcvt.*`; `th_vcvt_*` encoders already exist. Same
  shape as the arithmetic: two `vmov`s around one instruction.
- **`fcmp`** — `vcmp.f32` + `vmrs apsr_nzcv`. This needs its own condition
  mapping, and it is a **third** convention: VFP's compare sets flags like an
  ordinary compare of the two values with `V` for unordered, which is neither
  libsoftfp's signed three-way nor the DCP's C/Z/V-with-unsigned-conditions. The
  DCP compare work in `ir/gen/float.c` is the template; do not assume the
  mapping carries over.
- **Phase 5 proper — floats living in `s0-s15`.** Everything above still moves
  every value through GPRs, so a chain of float operations pays a `vmov` pair per
  op. Keeping values in VFP registers across statements is the real win and needs:
  - **`th_vldr`/`th_vstr`** — still do not exist, and without them FP spill and
    reload are impossible. The one genuinely new encoder.
  - `ir/vreg.c`'s `interval->use_vfp = (float_abi == ARM_HARD_FLOAT)` keyed on
    FPU capability instead of ABI, so softfp gets VFP *computation* with GPR
    *parameter passing* and `vmov` only at call boundaries — exactly GCC's
    `-mfloat-abi=softfp`.
  - a register class and the allocator work that implies.

### Later

- DCP `ddiv` + `sqrt` in the library. Needs `smmla`/`smmlar`/`smmul`/`smmulr`
  added to `thop_dsp.c` — currently absent (only `umull`/`umlal` exist).
- `ubfx` has no assembler token despite `thop_bitfield.c` having the encoder;
  `dcp_aeabi.S` works around it with shifts.
- `dcp_dclassify_m` (2 instructions) behind `__builtin_isnan`/`isinf`.
- **Teach the optimizer the inline path.** Several passes recognise FP work by
  matching the *call name* — `source/opt/flat/scalar/value_tracking.c`,
  `source/opt/flat/loop/const_sim.c`, `source/opt/ssa/cfg/branch.c`,
  `tcc_ir_is_pure_aeabi()`. Every operation moved inline stops being a call and
  therefore stops being recognised, so constant folding, purity and the
  compare-of-constants branch fold all quietly go dark under `-mfpu=rp2350` for
  `dadd`/`dsub`/`dcmp` and the float arithmetic. **Nothing fails when this
  happens** — the code is merely worse — which is why it needs a deliberate pass
  rather than waiting for a bug report. The fix is to match on the IR op plus
  operand constness instead of the symbol name.
- `__attribute__((interrupt))` so the compiler can enforce the preemptor-saves
  contract (only `naked` and `noinline` exist today).
- `libsoftfp` has no `__aeabi_l2d`/`ul2d`/`l2f`/`ul2f` at all — 64-bit-int→float
  still falls through to libgcc.
- **Make `libvfpv4sp` and `libvfpv5dp` self-contained too**, then add them to
  `SELF_CONTAINED_VARIANTS` in `lib/fp/Makefile`. They are missing 33 and 20
  `__aeabi_*` symbols respectively and only link today because something else on
  the command line supplies the soft-float fallbacks. `conv64.c` already exists
  for them; the rest is the same object-collision analysis done for rp2350.
- **Decide whether to move to the AEABI's own `cdcmp*` flag encoding.** Today
  neither `libsoftfp` nor `librp2350fp` is callable from GCC-compiled code, and
  a GCC object linked against either gets silently wrong `<`/`<=`. Switching
  means changing `ir/gen/float.c` + the backend's condition mapping *and* both
  libraries in one commit. Note this is now *only* about the library: the inline
  DCP compare already consumes RCMP's native encoding directly.
- `-mfloat-abi=hard` for float args/returns, as a separate opt-in.
- **Split `ir_op_is_implicit_call_ra()` by operand width.** It gates on
  `!(has_f<op> && has_d<op>)` — one answer for both widths — so an operation
  inline in one width and a call in the other is modelled as a call for both.
  With `dadd`/`dsub` and the float arithmetic in, FADD/FSUB now clear it and do
  collect the register-pressure win; FMUL, FDIV and FCMP are each inline in one
  width only and stay pessimised. Correct as-is, just conservative — and it must
  *stay* conservative if it is ever refactored, since the failure direction is a
  miscompile.
- **Give the fpv5 tables their emitters back.** `fpv5-sp-d16.c` and `fpv5-d16.c`
  now claim only `fadd`/`fsub`/`fmul`/`fdiv`, because that is all that has an
  emitter. Every other bit was turned off to close a real miscompile, not
  because the silicon lacks the op — `fpv5-d16` in particular has genuine
  double-precision hardware (`vadd.f64` and friends) that is now entirely
  unused. Wiring `FP_DOUBLE_IMPL_VFP` into `tcc_gen_machine_fp_mop()` alongside
  the DCP path would light it up.
- **Measure floats.** `bench_double.c` covers the double path; there is still no
  float kernel anywhere in the benchmark suite (`bench_math.c`'s "float_math" is
  an integer loop returning a constant), so inline VFP has correctness numbers
  (2803/2803) and a size number but no speed number.
- **Make the literal-pool flush check exact.** `THUMB_POOL_FLUSH_BUDGET` is a
  proxy — `code_size + pool_count*4` — carrying a 64-byte cushion, and it has
  already needed retuning once when inline DCP compares shifted where flushes
  land. The exact quantity is the span from the earliest pending entry's
  `patch_position` to where the pool would land; the obstacle is that entries are
  allocated before that field is set (`load_full_const`). Fixing the ordering
  would let the cushion go away.

---

## Traps

Recorded because each cost real time.

- **`-lsoftfp` finds `libsoftfp.so` (yaff) before `libsoftfp.a`.** Any test
  harness naming FP libraries with `-l` is probably testing libgcc. Use explicit
  archive paths.
- **`const T x;` in a header is a tentative definition, not a declaration.**
  Every includer gets its own zero-initialised copy and the real definition is
  dead. Two FPU capability tables were dead this way.
- **`cd lib/fp && make FPU=...` resolves `FP_CC` to host gcc** and fails on
  `-mcpu=cortex-m33`. Always go through `make cross` / `make fp-libs`.
- **Deleting a source leaves its `.o` in `lib/fp/build/<variant>{,-pic}/`**, and
  the archive rule globs `*.o` — stale objects produce "defined twice". Remove
  the build dirs when removing sources.
- **tcc's inline asm cannot satisfy ~12 `"r"` operands.** The DCP multiply needs
  seven scratch registers; use `.S` with fixed ABI registers.
- **A backend that masks an offset into an instruction instead of range-checking
  it produces a working-looking binary.** The literal-pool patcher did this for
  years; the symptom was an unrelated-looking HardFault in a function that had
  merely grown. Any `|= x & MASK` onto an encoded field wants an assertion.
- **pico-sdk hijacks every double operation unless you opt out.**
  `pico_set_float_implementation(... compiler)` does *not* imply the double
  equivalent; left unset, `pico_double_default` links the SDK's own DCP
  assembly with `-Wl,--wrap=__aeabi_dadd` and friends, so the compiler's FP
  runtime is never called. The symptom is that `--fp-lib softfp` and
  `--fp-lib rp2350fp` produce **byte-identical images** — the tell is a
  `__wrap___aeabi_dadd` symbol. Any TCC-vs-GCC double comparison made before
  `pico_set_double_implementation(... compiler)` was added to the benchmark
  target was measuring pico-sdk, not the compilers.
- **A `--skip-build` flag that reconstructs the build path itself will drift
  from the builder.** `run_benchmark.py --fp-lib rp2350fp --skip-build`
  re-flashed the softfp image and reported the two configurations as identical
  — a perfect null result with no error. Both paths now go through
  `benchmark_build_dir()`.
- **A test harness that caps its own output hides the shape of a failure.** The
  20 visible `FP FAIL` lines were all `add`, which read as "dadd is broken". The
  per-op histogram showed add/sub/mul/cmp/widen failing and div/conversions
  clean — i.e. the DCP ops and not the soft ones — which is a different
  diagnosis reached in one run instead of several.
- **`make cross 2>&1 | grep -E "error|warning:"` is not a build check.** It
  matches `-Werror` in the echoed command line, so a failing build reads as
  clean and you then spend a while debugging a stale binary. Use
  `make cross > log 2>&1; echo $?`, or grep for `\.c:[0-9]+:[0-9]+: error`. A
  missing `#include` for `thop_coproc.h` and later `thop_vfp.h` both hid this
  way; the giveaway the second time was `has_dadd` reading 1 while `has_fadd` in
  the *same struct literal* read 0.
- **A capability table is a claim about the backend, not the silicon.**
  `fpv5-d16.c` asserted every `has_*` bit with no emitter behind any of them,
  which un-modelled the r0-r3 clobber of calls that were still being emitted.
  Check a config table against real emitters before believing it.
- **Two tests compiling one source with different flags need different object
  paths.** `test_codegen_asm.py`'s `_compile` keyed the `.o` on the source name
  only, so the soft-float and hard-float `fp_select` cases raced under
  pytest-xdist. Invisible while both expected the same lowering; an
  intermittent failure the moment one started expecting VFP.
- **A branch to a local label needs `.type <label>, %function`.** tcc's
  assembler leaves branches to local labels as relocations and emits no `$t`
  mapping symbols, so ld has nothing to tell it the target is Thumb and rejects
  the `R_ARM_THM_CALL` with *"Unknown destination type (ARM/Thumb)"*. It only
  bites unconditional `b`; conditional branches become `R_ARM_THM_JUMP19`,
  which ld resolves without the check — which is why the earlier `bvs
  .Ldcmp_nan` never showed the problem.
- **The `__aeabi_cdcmp*` flag convention in this tree is tcc's, not the
  AEABI's.** Signed conditions on a `-1/0/1/2` result, not the standard ZCV
  encoding read with unsigned conditions. Copying a sequence from pico-sdk or
  libgcc gets this backwards and the failure is silent until hardware.
- **A differential test cannot be mutation-tested by mutating its input.**
  Mutating `test_coproc.S` proved nothing because tcc and gcc both saw the
  mutated source; the encoder had to be mutated instead.
- **QEMU has no RP2350 machine.** Hardware is the only DCP validation path.
- **Byte signatures used to identify a linked implementation go stale** the
  moment that implementation is edited.

## Verification

```bash
# hardware (board on 192.168.0.113, user mateusz)
tests/benchmarks/run_fp_conformance.py 192.168.0.113                      # softfp baseline
tests/benchmarks/run_fp_conformance.py 192.168.0.113 \
    --fp-lib rp2350fp --mfpu rp2350 --allow-ftz                           # DCP gate

make cross fp-libs -j16              # incl. check-self-contained on rp2350
make test-aeabi-host                 # host FP conformance, ~0.3 s
make test -j16                       # IR tests
make test-asm -j16                   # assembler, incl. coprocessor encodings
cd tests/unit/arm/armv8m && make all -j16 && ./build/run_unit_tests
cd tests/unit/arm/armv8m && make run-backend -j16
cd tests/ir_tests && python3 -m pytest test_qemu.py -q -n16

# link the hardware image without a board (catches every Phase-3b regression)
python3 -c 'import sys; sys.path.insert(0,"tests/benchmarks"); \
  from run_fp_conformance import build_image; \
  print(build_image("TCC","1","rp2350fp","rp2350",True))'
```
