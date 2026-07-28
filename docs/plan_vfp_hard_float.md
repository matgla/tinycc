# Plan: Add ARMv8-M hard-float VFP support (`-mfloat-abi=hard`)

## Context

The YasOS TinyCC fork already parses `-mfloat-abi=hard` and `-mfpu=…`, sets `TCCState::float_abi` / `fpu_type`, and even configures `architecture_config.fpu` and VFP register allocation in `arm_init()`.  The VFP Thumb encoder (`arch/arm/thumb/thop_vfp.c`) is complete for the operations we need.

What is missing is the **codegen path**: `tcc_gen_machine_fp_mop()` in `arm-thumb-gen.c` unconditionally lowers every FP IR operation (`FADD`, `FSUB`, `FMUL`, `FDIV`, `FCMP`, `FNEG`, `CVT_ITOF`, `CVT_FTOI`, `CVT_FTOF`) to soft-float `__aeabi_*` library calls.  As a result, `fp_select.c` compiled with `-mfloat-abi=hard -mfpu=fpv5-sp-d16` still calls `__aeabi_fadd`/`__aeabi_dadd`/`__aeabi_fmul` and passes floats in integer registers.

The goal is to make `-mfloat-abi=hard` emit VFP instructions and use the VFP register bank for FP values, parameters, and return values, while keeping soft-float behavior unchanged.

> **Note (2026-07-28):** the source tree was restructured — the backend is now
> `source/backend/arch/arm/thumb/arm-thumb-gen.c` (not `libs/tinycc/…`); the IR
> layer (RA, codegen driver, vreg) lives under `ir/`.  Paths below reflect the
> new layout.  The state summary was refreshed after auditing the live code.

## Current state summary

| Layer | State |
|---|---|
| Command-line parsing | `-mfloat-abi=hard` and `-mfpu=fpv{4,5}*dp{16,32}` parsed into `float_abi` / `fpu_type` — ✅ |
| Feature resolution | `thumb_resolve_features()` maps `-mfpu=…` to `vfp_sp` / `vfp_dp` / `fp_armv8` bits — ✅ |
| VFP encoder | `thop_vfp.c`: arith/cmp/neg/cvt/vmov/vpush/vpop/vmrs, **plus `th_vldr`/`th_vstr`** (added 2026-07-28) — ✅ |
| Allocator hint | `ir/vreg.c:289` sets `interval->use_vfp = (float_abi == ARM_HARD_FLOAT)`; `tcc_ir_vreg_type_get` maps float/double → `LS_REG_TYPE_FLOAT`/`LS_REG_TYPE_DOUBLE` — ✅ |
| RA float bank | `ir/regalloc.c:1846-1883` allocates `s0-s15` for `LS_REG_TYPE_FLOAT` and even `s`-pairs for `LS_REG_TYPE_DOUBLE`, tagged `LS_VFP_REG_BASE (0x40) + n` — ✅ (producer only) |
| FPU config / bank size | `arm_determine_fpu_config()`; `float_registers_{for,map}_for_allocator` set when hard-float — ✅ |
| SP arith emission | `tcc_gen_machine_fp_mop()` → `thumb_emit_vfp_arith_mop()` emits `vadd/vsub/vmul/vdiv.f32` when `float_abi != soft` — ✅ but **GPR-shuffled** (softfp style; operands move through GPRs, floats never *live* in `s`-regs) |
| Return classification | `gfunc_sret():4316` already branches on `ARM_HARD_FLOAT` for float/HFA returns — ✅ |
| **Gap — MOP VFP-awareness** | `LS_IS_VFP_REG`/`LS_VFP_REG_BASE` are **never decoded** downstream: `mach_ensure_in_reg`/`mach_get_dest_reg`/`mach_writeback_dest`/`thumb_is_hw_reg` treat `r0≥0x40` as a bogus GPR. No `vldr`/`vstr` spill/reload wired. So RA-allocated VFP operands cannot be consumed. |
| **Gap — AAPCS args** | `arm_aapcs.c` / `arm-thumb-callsite.c` / `build_register_arg_moves` are GPR-only; float/double args go to `r0-r3`/stack, never `s0-s15`/`d0-d7`. `TCCAbiArgLoc` has no VFP field. |
| **Gap — param homing** | `tcc_ir_register_allocation_params` (`ir/codegen.c:197`) sets `incoming_reg0` to a GPR index; prolog drops any `≥16` reg. `use_vfp` is set but unused for the ABI edge. |
| **Gap — return regs** | `tcc_gen_machine_return_value_mop:10226`, `handle_return_value_mop:12657`, `mark_return_value_incoming_regs (codegen.c:291)` hardcode `R0/R1`, never `s0/d0`. |

## Goal

When `float_abi == ARM_HARD_FLOAT` and the selected FPU supports the operation:

1. FP values live in VFP registers (`s0-s15` for single, `d0-d7` for double on `fpv5-d16`).
2. FP arithmetic/compare/negate/conversion lower to VFP instructions instead of `__aeabi_*` calls.
3. FP function arguments and return values follow the AAPCS hard-float convention (`s0-s15` / `d0-d7`, then stack).
4. Spills, reloads, and moves between GPR and VFP registers use `vldr`/`vstr`/`vmov`.
5. Existing soft-float (`-mfloat-abi=soft` / `softfp`) output is byte-for-byte unchanged.

## ⚠️ Key finding (2026-07-28) — the VFP≡GPR accident

Audit + `DBG_VFP` instrumentation of the linear scan established the real
dynamics, which **differ from this plan's original assumption** that the
allocator does not use VFP registers:

- In hard-float the RA **does** type every float vreg `LS_REG_TYPE_FLOAT`
  (`reg_type=1`, `fpmap=0xffff`) and **does** assign it a VFP register
  `LS_VFP_REG_BASE(0x40)+n` (`ir/regalloc.c:1846`).
- But codegen never decodes `0x40+n`.  `thumb_emit_vfp_arith_mop` passes it to
  `th_vmov_gp_sp`, whose encoder masks the register to its low 4 bits — so
  **VFP-reg-`n` silently aliases GPR-`rn`**.  Because *defs and uses* both mask
  identically, the float effectively lives in `rn` and shuffles through fixed
  `s0/s1`.  Output is **correct but softfp-quality**, and `-mfloat-abi=hard`
  is presently **byte-identical to `-mfloat-abi=softfp`**.
- **Latent hazard:** in a *mixed* int+float function, "VFP-reg-4" and "GPR-r4"
  both resolve to `r4`, so an int temp and a float temp that are simultaneously
  live can clobber each other.  Harmless *today* only because the default test
  ABI is softfp (floats → `LS_REG_TYPE_INT` → GPR, no VFP numbers ever appear).
  `make test` runs softfp, so **it does not cover the hard-float VFP path** —
  Phase 2+ needs its own hard-float execution gate.

**Consequence for sequencing:** Phases 2/3/4 are **coupled**, not independent.
The moment codegen treats `0x40+n` as a real `sN` (Phase 2), the VFP≡GPR
equivalence breaks, so the ABI edges must move in the same change: params
arriving in `r0-r3` need `r→s` moves at entry (or Phase 3's real `s`-reg
passing) and returns need `s→r0` (or Phase 4's `s0`).  Plan to land Phase 2+3+4
as one coherent hard-float-ABI change, gated by a QEMU hard-float execute test.

## ✅ Single-precision VFP register class landed (2026-07-28) — via `MACH_OP_VFP_REG`

A first approach-A attempt (preserve the `0x40` marker through
`machine_op_from_ir`) hit a wall: the codegen represents registers as a 5-bit
`PREG` field (`PREG_NONE=0x1F`, `PREG_SPILLED=0x20`, `& PREG_REG_NONE`
everywhere; ~31 masking sites, ~45 `<=R12`/`<16` guards, 78 `thumb_is_hw_reg`
callers), so a `0x40+n` GPR-space number gets masked back to `rn` in many
independent paths — whack-a-mole.  That attempt was reverted.

**Resolution — option 2: a distinct `MACH_OP_VFP_REG` operand kind.**  Floats
reach codegen as `MACH_OP_VFP_REG` (`u.reg.r0` = s-register 0-31), a different
kind from GPR `MACH_OP_REG`.  A VFP operand can therefore never match a
`kind == MACH_OP_REG` test, so no GPR-path mask ever sees it — and every
unhandled path hits a `default: tcc_error` (loud crash, not a silent
miscompile).  Soft/softfp never produce the kind (`use_vfp=0` → floats are
`LS_REG_TYPE_INT`), so their output is byte-identical.  Landed changes:
- `ir/machine_op.{h,c}` — the new kind; emitted for `LS_IS_VFP_REG` allocations.
- `ir/vreg.c` — hard-float doubles → `DOUBLE_SOFT` (confine VFP to singles).
- `ir/regalloc.c` — a float live across a call **must spill**: every allocatable
  VFP reg (s0-s13) is caller-saved and there are no callee-saved VFP regs in the
  map, so `crosses_call` floats get a stack slot.
- `arm-thumb-gen.c` — VFP cases in `mach_ensure_in_reg` / `mach_get_dest_reg` /
  `mach_writeback_dest` (GPR↔VFP `vmov` bridge), `fp_mop_load_arg`, `assign_mop`,
  `store_mop`/`load_mop` (delegate a VFP src/dest to the assign path), the arith
  emitter (reserved `s14/s15` scratch under hard-float), param homing (GPR→VFP
  `vmov`), and reserving `s14/s15` in `arm_init`.

Verified: `make test` (13791 pass, soft/softfp byte-identical), `make ut` green,
and 20 hard-float QEMU execute tests (5 files × 4 opt levels) covering
arith/neg/compare/cvt, mixed int+float params (the old miscompile — now correct),
memory/arrays/globals/pointer-deref, ternary select, function pointers, >4 stack
float args, and float loops/dot-product/polynomial.

**Still soft at the ABI boundary (approach A):** float args/returns still cross
calls in GPRs.  Real `s`-register argument passing (approach B — needs a float
flag through `arm_aapcs.c`) and native VFP emission (skip the GPR round-trip)
are follow-ups.  Doubles remain soft.  Loud-crash paths not yet exercised
(`lea_mop`, `select_emit_inline`, `prefetch`, complex) will `tcc_error` rather
than miscompile if hit — handle as found.

## Approach B (real s-register ABI) — investigation (2026-07-28)

Native single-precision arithmetic landed (commit 2679b659): `addf` is now
`vmov s0,r0; vmov s1,r1; vadd.f32 s2,s0,s1; vmov r0,s2` — the remaining moves are
the soft-ABI argument delivery (r0/r1→s0/s1) and return (s2→r0).  Removing those
is approach B.  Attempted the **return-in-s0** half first (it has no variadic
complication) and found the ordering constraint that blocks it:

**Return-in-s0 requires native FNEG / CVT_ITOF first.**  A float-returning
function whose body is a single FP op tail-calls the soft helper — e.g.
`negf` → `b.w __aeabi_fneg`, `float f(int){return (float)x;}` → `__aeabi_i2f` —
and those helpers return the float in **R0**, not s0.  So the return ABI can't
move to s0 until FNEG and int→float conversion emit **natively** (vneg.f32 /
vcvt.f32.s32).

**The blocker for native FNEG/CVT is the FPU config, by design.**  The lowering
gate `ir_put_soft_call_fpu_if_needed` (ir/gen/softfloat.c) keys ONLY on
`architecture_config.fpu` capability bits, deliberately ignoring `float_abi`
(pinned by `test_fpu_gate_ignores_float_abi`).  The **fpv5-sp-d16 config
under-advertises**: `has_fadd/fsub/fmul/fdiv` are set but `has_fneg`, `has_fcmp`,
`has_itof`, `has_ftoi` are **not**, even though the silicon has `vneg`/`vcmp`/
`vcvt`.  A `float_abi`-gated shortcut in the gate violates the design and breaks
those unit tests (tried, reverted).

**Decision needed (affects softfp):** completing the fpv5-sp-d16 caps (+ the
matching backend native emitters for FNEG/CVT/FCMP) makes **both** softfp and
hard-float use the FPU natively for those ops — correct (softfp = FPU + GPR
passing) but it **changes softfp output**, so golden-IR / test-codegen-asm
expectations regenerate.  FCMP additionally needs the frontend to switch from
the __aeabi unsigned branch conditions to VFP's signed conditions.

**Approach-B roadmap:**
1. ~~Return-in-s0~~ **DONE (commit 9b483f64).**  Avoided the FPU-caps/softfp-churn
   route entirely: instead of native FNEG/CVT, (a) keep a float-returning tail
   call non-tail under hard-float (`ir_tail_call_returns_hard_float`, regalloc.c)
   so the result flows through `return_value_mop`, and (b) have the caller read
   the return from s0 EXCEPT when the callee is a soft `__aeabi_*` helper — those
   return in R0 (`mach_callee_is_aeabi`, name-based, reserved namespace).  `addf`
   now ends `vadd.f32 s2,s0,s1; vmov.f32 s0,s2`.
### ✅ Arg passing LANDED (2026-07-28, commit 4a69cab2) — approach B complete

Second attempt succeeded.  The ">4-float callee bug" root cause was
`tcc_ir_avoid_spilling_stack_passed_params` (ir/codegen.c): it recomputed
"stack-passed" params with the GPR-only `argno<=3` rule, so float params 5+ had
their correct linear-scan s4/s5 allocation force-reset to `PREG_NONE` in the
`ir->ls` table, which the backend copy loop then propagated over the IR
intervals — both collapsed onto a shared bogus `[fp+0]` slot.  Fixed by
mirroring the VFP argument counter there.  Also fixed while landing: the three
stack-arg placement paths + `is_simple_imm_stack_arg` treated "not
TCC_ABI_LOC_REG" as "stack", phantom-storing VFP-loc args at `[sp+0]` (a real
clobber for mixed calls with genuine stack args) — all now skip
`TCC_ABI_LOC_VFP_REG`.  New `fp_hard_absplit_exec.c` covers mixed
GPR+stack+VFP banks.  `addf` is now full AAPCS-VFP:
`vadd.f32 s2,s0,s1; vmov.f32 s0,s2; bx lr`; `sum6f` is a pure vadd chain over
s0-s5.  Gate: 24 execute tests, make test 13803, make ut green.

**Hard-float single-precision ABI is now COMPLETE** (args s0-s15, return s0,
native arith, VFP register class).

### ✅ `.ARM.attributes` fixed (2026-07-28, commit 405094bb)

The section was a hardcoded **ARMv6** blob patched at fixed byte offsets, so
every ARMv8-M binary misdescribed itself (`Tag_CPU_arch: v6`, `Tag_ARM_ISA_use:
Yes` — M-profile has no ARM ISA, `Tag_FP_arch: VFPv2` regardless of `-mfpu`);
`Tag_ABI_HardFP_use` and `Tag_CPU_arch_profile` were absent.  `Tag_ABI_VFP_args`
itself was already correct (the non-hard path overwrote the tag).  It is now
built programmatically from `arm_get_eabi_attrs()`, and the float trio matches
`arm-none-eabi-gcc` byte-for-byte for cortex-m33 + fpv5-sp-d16:

| ABI | Tag_FP_arch | Tag_ABI_HardFP_use | Tag_ABI_VFP_args | e_flags |
|---|---|---|---|---|
| soft | *(absent)* | *(absent)* | *(absent)* | soft-float |
| softfp | FPv5/FP-D16 for ARMv8 | SP only | *(absent)* | soft-float |
| hard | FPv5/FP-D16 for ARMv8 | SP only | VFP registers | hard-float |

Deliberate differences from GCC: `Tag_ABI_enum_size` stays `int` (tcc's actual
enum ABI; arm-none-eabi-gcc defaults to short enums — worth a separate audit)
and `Tag_DSP_extension` is not claimed.  Pinned by
`tests/ir_tests/test_elf_attributes.py`.

**Known gap:** attributes are emitted only at *link* time — tcc object files
carry no `.ARM.attributes` at all, so GNU `ld` cannot ABI-check them.  Adding it
per-object needs care (tcc's linker creates its own section and would then see
one per input object).

### ✅ Float ABI selectable end-to-end (2026-07-28, commit d98c8368)

The harness hardcoded `-mfloat-abi=soft` for the *link*, so hard-float tests could
only ever be self-contained.  Now one `FLOAT_ABI`/`FPU` knob drives compiler
flags **and** library selection:

- `tests/ir_tests/qemu/mps2-an505/Makefile` — `FLOAT_ABI=soft|softfp|hard`.
  The in-tree newlib is soft-only, so other ABIs use the toolchain multilib
  (`thumb/v8-m.main+fp/{softfp,hard}`), which ships libc/libm/libgcc/librdimon/crt.
- `build_newlib.sh <abi> [fpu]` — per-ABI newlib tree, for targets the multilib
  does not cover.
- `qemu_run.py` — `CompileConfig.float_abi/fpu` → make vars, defaulting from
  `TCC_FLOAT_ABI` / `TCC_FPU`.
- `make test-fp FLOAT_ABI=hard` — re-run the float tests under one ABI.
- `fp_libm_exec.c` — real libm calls, run across all 3 ABIs × 4 opt levels.

**Two bugs it surfaced immediately:**
1. `lib/builtin.c`'s freestanding C float fallbacks (`fabsf`, `floorf`,
   `fmaxf`, …) live in `armv8m-libtcc1.a`, built with the compiler's default
   ABI, and **preceded libm in the link** — so a hard-float program got the
   soft-ABI `fabsf` and `fabsf(-3.5f)` returned `-3`.  Fixed by putting libm
   before libtcc1.  (`__aeabi_*` helpers are unaffected: the RTABI fixes them to
   the base PCS regardless of the caller, which is why `lib/fp` stays soft-built.)
2. `USE_NEWLIB_BUILD=0` requested `libc_g.a`/`libm_g.a` unconditionally though
   only the local newlib has debug variants — now `wildcard`-guarded.

### ✅ Doubles in d0-d7 (2026-07-28, commit 091f22da) — hard-float ABI COMPLETE

The last correctness gap.  Verified against `arm-none-eabi-gcc`: AAPCS-VFP
passes doubles in `d0-d7` **even on a single-precision-only FPU** — the ABI says
where arguments live, not which arithmetic exists, so the callee unpacks `d0`
into a GPR pair to call `__aeabi_dadd`.  Doubles therefore stay non-VFP-resident
internally and only the ABI boundary moved.  `addd()` now assembles
byte-identically to GCC's for the same flags.

- **Allocation is a bitmap, not a counter** — AAPCS back-fills: a float takes
  the lowest free s-register, a double the lowest free *even-aligned* pair.
  `mixd(int,double,float,double)` → `d0`/`s2`/**`d2`**, with `s3` left free.
  Once one FP argument spills to the stack, the VFP bank closes for all later
  ones.  Mirrored in all three places that must agree (`arm_aapcs.c`,
  `register_allocation_params`, `avoid_spilling_stack_passed_params`).
- **Real miscompile fixed:** the callee's `vmov r0,r1,d0` unpack ran *before*
  the GPR parallel move, clobbering a leading `int` parameter still sitting in
  r0.  Unpacks are now deferred until after that move.
- **Encoder gating corrected:** 64-bit `vmov`/`vldr`/`vstr` were gated on
  `vfp_dp`, which made the double ABI unencodable on `fpv5-sp-d16`.  They are
  *data movement*, not double arithmetic, and are legal whenever the register
  file exists (confirmed with `arm-none-eabi-as`) — now gated on `vfp_sp`.

`make test-fp` is **96/96 under all three ABIs** (was 93/96 for hard).

**The hard-float ABI is now complete**: args in s0-s15/d0-d7, returns in s0/d0,
native single-precision arithmetic, correct ELF/EABI marking, and libc/libm
interop under soft, softfp and hard.

### ✅ CI wiring (2026-07-28, commit 4bdb5528)

- **QEMU** (`build-and-test`): a "Float ABI matrix" step runs the float tests
  under **softfp and hard** in addition to `make test`'s soft coverage — this is
  what makes the libm interop tests meaningful, since they link libc/libm/libgcc
  built for the same ABI.  Guarded: warns and skips if the image's
  arm-none-eabi lacks the matching multilib, rather than failing on container
  contents.  Dry-run verbatim: 96 passed under both.
- **RP2350** (`metrics`, self-hosted): runs `run_fp_conformance.py` in two
  configurations — the soft-float baseline (control: the identical IEEE-754
  vectors run under QEMU as `421_fp_conformance.c`, so the two must agree) and
  **inline DCP/VFP codegen**, which QEMU cannot model at all.  Placed after the
  metrics steps so a conformance failure still leaves perf/codesize recorded.

Remaining follow-ups (none are hard-float correctness gaps):
- **hard-float on RP2350 silicon** — needs the pico-sdk rebuilt for the hard
  ABI; deliberately not attempted as untested CMake plumbing.
- native `vadd.f64` on a DP-capable FPU (`-mfpu=fpv5-d16`).
- `.ARM.attributes` in object files (link-time only today).
- a known-broken `fmaxf`/`fminf` — wrong under *soft* too (returns a double bit
  pattern); pre-existing and unrelated to the ABI work.

### Historical: first activation attempt (2026-07-28) — caller works, callee >4-float bug, REVERTED

Wired the full activation on top of the scaffolding and got the **caller** side
working perfectly — `addf` compiled to `vadd.f32 s2,s0,s1; vmov.f32 s0,s2; bx lr`
with args arriving in s0/s1.  18/20 hard-float execute tests passed.  The 2
failures were `fp_hard_call_exec` (a 6-float-arg `sum6`), and chasing it exposed
a chain of callee/RA issues — each fixed, the next appeared:
- `params.c` needs `desc.is_float` too (else the callee classifies floats as GPR).
- The RA models a single-precision `FADD` as an **implicit call** (`has_dadd=0`),
  so `crosses_call` is a false positive → added `crosses_real_call` (real
  FUNCCALL only) for the VFP-spill decision.
- The RA `precolored` path and the `pos < 4` param cap are GPR-centric (mask into
  `int_free`, cap at 4 arg regs) — extended for VFP.
- **Unresolved:** for >4 float params the linear scan *does* allocate them to
  s4/s5 (fp_free decrements), but the allocation does **not persist** to
  `IRLiveInterval.allocation.r0` (stays `PREG_NONE`) by `machine_op_from_ir`
  time, so P4/P5 collapse to an unallocated stack slot `[fp+0]` (they collide).
  `tcc_ir_register_allocation_params` runs twice (codegen.c:516 + backend
  regalloc.c:649) around the scan; the float-param incoming/allocation handling
  is not robust to that double pass.  The `≤4`-float-arg case works.

Reverted the activation (kept return-in-s0 + inert scaffolding, all green).
**Next attempt must first understand the two `register_allocation_params` passes
vs the linear scan, and make float-param VFP allocation persist** (the scan
already allocates correctly).  A cap-at-4 avoids the bug but violates AAPCS.

2. **Argument passing in s0-s15** (next, atomic — caller + callee together):
   - `tccabi.h`: `TCCAbiArgDesc.is_float`; new `TCC_ABI_LOC_VFP_REG` loc kind;
     `TCCAbiCallLayout.next_vfp_reg` + `is_variadic`.
   - `arm_aapcs.c`: place `is_float && size==4` args in s0-s15 when hard-float
     and not variadic; else the existing GPR/stack path.
   - Caller: `build_reg_move_32bit` / `build_register_arg_moves` move a float arg
     into its s-register (VFP-resident → `vmov.f32`; else materialize).
   - Callee: `ir/codegen.c` param homing + `ir/gen/params.c` set the float
     param's incoming location to its s-register.
   - **Variadic:** a variadic callee passes ALL args (named + `...`) in GPRs
     (base standard), so detect variadic at the call site (callee `Sym` type =
     `FUNC_ELLIPSIS`) and fall back to the soft layout.  Callee side already
     knows via `sym->f.func_type`.
3. `.ARM.attributes` `Tag_ABI_VFP_args` audit; doubles in d0-d7 (later).

## Progress log

- **2026-07-28** — Phase 1 (encoders): added `th_vldr` / `th_vstr` to `thop_vfp.c`
  (single + double precision, signed SP-relative imm8 offset, U-bit handling),
  declared in `thop_vfp.h`.  12 unit tests in `test_thop_vfp.c`, all encodings
  cross-checked against `arm-none-eabi-as`.  `make ut` green; no codegen path
  calls them yet, so soft-float output is byte-identical.  These are the
  spill/reload primitive the in-tree note at `arm-thumb-gen.c:9537` flagged as
  missing.

- **2026-07-28** — Safety net (chosen before the Phase 2-4 rewrite, since
  `make test` runs softfp and cannot cover the hard-float path): added a
  hard-float QEMU execution gate in `tests/ir_tests/test_qemu.py`:
  - `test_hard_float_execution` / `fp_hard_sp_exec.c` — pure single-precision
    (arith, neg, compare, cvt, float params/returns).  **Green at
    -O0/-O1/-O2/-Os**; the regression net that must stay passing through the
    rewrite.
  - `test_hard_float_mixed_xfail` / `fp_hard_mixed_exec.c` — mixed int+float
    params, **strict-xfail**.  Confirmed the VFP≡GPR accident is a *live*
    miscompile (not merely latent): `mix(int,float,int)` computes `n*n` instead
    of `x*x` at -O0/-Os because the float param aliases an int GPR (folded away,
    so masked, at -O1/-O2; volatile args force the call at every level).  Flips
    to xpass when Phase 2-4 lands → promote into the green gate, drop the xfail.

  **Next: Phase 2+3+4 (coupled)** — decode `LS_VFP_REG_BASE` in the MOP/mach
  layer as a real `sN`, emit native VFP, move params/returns to `s`-regs, wire
  `vldr`/`vstr` spill+reload.  Gate every step on the hard-float execute tests
  above + `make test` (soft/softfp byte-identity) + `make ut`.

## Concrete implementation design (approach A — 2026-07-28)

Approach A keeps the **soft argument layout** (floats cross call boundaries in
GPRs) but makes floats *live* in real VFP registers internally, with explicit
GPR↔VFP bridges at the edges.  It fixes the confirmed mixed int/float
miscompile and is confined to the ARM backend (no frontend / `arm_aapcs.c`
changes).  Real `s`-register argument passing (approach B) is a later phase.

**Everything is gated on `is_vfp_reg(r)` (`r >= LS_VFP_REG_BASE`) or
`float_abi == ARM_HARD_FLOAT`.** In soft/softfp, `use_vfp=0` so floats are
`LS_REG_TYPE_INT` (GPR, `r < 0x40`); no VFP number ever appears → every new
branch is dead → soft/softfp output stays byte-identical.  **This must land
atomically** — a half-migrated state where some sites treat `0x40+n` as a real
`sN` while others still mask it to `rn` will miscompile.

Sites (all in `source/backend/arch/arm/thumb/arm-thumb-gen.c` unless noted):

1. **Reserve FP scratch** — `arm_init` (`2657`): when hard-float, mask `s14/s15`
   out of `float_registers_map_for_allocator` (→ `0x3fff`) so the RA never
   allocates them; use them as the emitters' fixed VFP scratch (`VS0=14`,
   `VS1=15`).  Safe: soft-ABI float args use GPRs, so `s14/s15` are never ABI
   argument slots here.
2. **Decode helpers** — `is_vfp_reg(r)=(r>=0x40&&r<0x60)`, `vfp_num(r)=r-0x40`
   (wrap `LS_IS_VFP_REG`/`LS_VFP_REG_NUM`).
3. **Core operand bridge** — `mach_ensure_in_reg` (`512`): VFP-resident
   `MACH_OP_REG` → alloc GPR scratch, `vmov gpr, sN`, return gpr.
   `mach_writeback_dest` (`687`): VFP-resident dest → `vmov sN, reg`.
   `mach_get_dest_reg` (`657`): VFP-resident → GPR scratch (compute in GPR,
   writeback bridges to `sN`).  (Makes every GPR-centric load/store/move handle
   float operands transparently; `MACH_OP_SPILL` of a float loads/stores its
   bits via GPR, so no `vldr`/`vstr` needed for correctness.)
4. **Native FP emitters** — `thumb_emit_vfp_arith_mop` (`9543`) + FNEG / FCMP /
   CVT_ITOF / CVT_FTOI / CVT_FTOF paths in `tcc_gen_machine_fp_mop`: resolve
   each operand to an `s`-reg (VFP-resident → `vfp_num`; else materialize into
   `VS0`/`VS1` via `vmov`/`vldr`), emit `v<op>.f32 sd,sn,sm` (or `vneg`/`vcmp`+
   `vmrs`/`vcvt`), then bridge the dest out if it is not VFP-resident.  Replaces
   the current fixed-`s0/s1` shuffle (which is unsafe once `s0/s1` are
   allocatable floats).
5. **Param homing** — prolog parallel-move (`10674`): the `alloc_r0 <= R12`
   guard drops VFP homes today (root cause of the `mix` miscompile).  Add a
   GPR→VFP move for float params: `vmov s_alloc, r_incoming`.  VFP dests are
   never GPR sources, so emit them after the GPR parallel move (always acyclic).
6. **Return** — `tcc_gen_machine_return_value_mop` (`10226`) already routes
   through `mach_ensure_in_reg`, so the core bridge (step 3) makes `vmov r?,sN;
   mov r0,r?` fall out for free; verify no direct `u.reg.r0` read bypasses it.
7. **Call-arg marshalling** — `build_register_arg_moves` (`12248`): a float arg
   in `sN` must reach its GPR arg register via `vmov rk, sN` (bridge) rather
   than a masked GPR `mov`.
8. **assign/copy** — `tcc_gen_machine_assign_mop`: float `MACH_OP_REG`↔REG via
   `vmov.f32`; REG↔SPILL via the GPR bridge; REG↔GPR via `vmov_gp_sp`.

Validation: `test_hard_float_execution` green + `test_hard_float_mixed_xfail`
flips to xpass (→ promote to the green gate, drop the xfail) + `make test`
(soft/softfp byte-identical) + `make ut`.

## Approach

A single incremental approach: teach the existing MachineOperand-based backend (`arm-thumb-gen.c`) to handle `MACH_OP_REG` operands whose register is a VFP register, and branch `tcc_gen_machine_fp_mop()` to VFP instruction emission when in hard-float mode.

This is preferred over rewriting the legacy (non-MOP) FP path because:
- The IR pipeline already routes FP ops through `tcc_gen_machine_fp_mop()`.
- The VFP encoder is already available and unit-tested (`test_thop_vfp.c`).
- The MOP abstraction already distinguishes operand kind, register, spill, immediate, etc.

## Phases

### Phase 1 — VFP operand materialization helpers

Add small helpers in `arm-thumb-gen.c` analogous to the existing `mach_ensure_in_reg()` family, but for VFP registers:

- `vfp_ensure_in_sreg(MachineOperand src, int sreg)` — load SPILL/IMM/SYMBOL into VFP single register `sreg`.
- `vfp_ensure_in_dreg(MachineOperand src, int dreg)` — same for double register pair / `dreg`.
- `vfp_spill_sreg(int sreg, int frame_offset)` / `vfp_reload_sreg(...)` — `vstr`/`vldr` with SP-relative addressing.
- `vfp_move_ss(int dst, int src)` / `vfp_move_dd(...)` — `vmov.f32`/`vmov.f64`.
- `vfp_mov_gp_sp(int rt, int sn, int to_arm)` / `vfp_mov_2gp_dp(...)` — GPR ↔ VFP moves for parameter/return edges and int↔float conversions.

Key files:
- `libs/tinycc/arm-thumb-gen.c`

Tests:
- `tests/unit/arm/armv8m/test_thop_vfp.c` already covers the encoders; extend it with a few GPR↔VFP move cases if gaps are found.
- Add `tests/ir_tests/asm/fp_hard_basic.c` and a passing assertion in `test_codegen_asm.py` that `vadd.f32`/`vmul.f32` appear.

### Phase 2 — Hard-float branch in `tcc_gen_machine_fp_mop()`

At the top of `tcc_gen_machine_fp_mop()`, add:

```c
if (float_abi == ARM_HARD_FLOAT && architecture_config.fpu->has_fadd)
  return tcc_gen_machine_fp_mop_hard(src1, src2, dest, op, is_complex);
```

Implement `tcc_gen_machine_fp_mop_hard()`:

| IR op | VFP sequence |
|---|---|
| `FADD`/`FSUB`/`FMUL`/`FDIV` | ensure operands in `s/d` regs, emit `vadd.f32`/`vsub.f32`/`vmul.f32`/`vdiv.f32` (or `.f64`), write back |
| `FNEG` | `vneg.f32` / `vneg.f64` |
| `FCMP` | `vcmp.f32` / `vcmp.f64`, then `vmrs apsr_nzcv, fpscr` |
| `CVT_ITOF` | `vcvt.f32.s32` / `vcvt.f64.s32` (unsigned variants via `u32`) |
| `CVT_FTOI` | `vcvt.s32.f32` / `vcvt.s32.f64` (unsigned/truncation variants) |
| `CVT_FTOF` | `vcvt.f64.f32` / `vcvt.f32.f64` |

Guard each operation by the FPU config flags (`has_fadd`, `has_fmul`, `has_ftoi`, etc.); fall back to the existing soft-float path if the selected FPU lacks support.

Key files:
- `libs/tinycc/arm-thumb-gen.c`

Tests:
- Extend `tests/ir_tests/asm/fp_select.c` or add `fp_hard_ops.c` covering `+`, `-`, `*`, `/`, compare, negate, int↔float, float↔double.
- Update `test_fp_hard_float_uses_vfp` to pass and add `test_fp_hard_float_all_ops`.

### Phase 3 — AAPCS hard-float parameter passing

Modify the call-layout builder (`thumb_build_call_layout_from_ir()` and related helpers) so that when `float_abi == ARM_HARD_FLOAT`:

- `float` args use `s0, s1, …` up to `s15`.
- `double` args use `d0, d1, …` up to `d7` (each consumes two single slots).
- Mixed int/FP args consume independent GPR and VFP register banks (AAPCS rule).
- Variadic functions continue using the soft-float layout (AAPCS requirement).
- Once VFP registers are exhausted, FP values spill to the stack argument area.

Also update the caller side that marshals `FUNCPARAM` operands into argument locations so it knows how to move a VFP-register operand into `sN` (`vmov` or direct if already allocated there).

Key files:
- `libs/tinycc/arm-thumb-gen.c` (call layout and param marshalling)
- Possibly `arch/arm/arm_aapcs.c` if the layout logic is split there

Tests:
- `tests/ir_tests/asm/call_fp_args.c`: functions with `float`/`double`/mixed int+FP args; assert the right `vmov`/`vldr` into `s0-s7`/`d0-d3` and no `__aeabi_*` calls.

### Phase 4 — Hard-float return values

Update `tcc_gen_machine_return_value_mop()` and `gfunc_sret()`:

- `float` return → `s0`.
- `double` return → `d0`.
- Callee writes directly to `s0`/`d0`; caller reads from there.

Key files:
- `libs/tinycc/arm-thumb-gen.c`

Tests:
- `tests/ir_tests/asm/call_fp_return.c`.

### Phase 5 — Spill / reload / prolog / epilog

Ensure the register allocator's VFP register bank (`float_registers_for_allocator`) is actually used for FP vregs when `use_vfp` is set, and that spills are emitted via `vstr`/`vldr`:

- Verify `ir/regalloc.c` allocates VFP registers to intervals with `use_vfp == 1`.
- Verify spill code in `arm-thumb-gen.c` emits `vstr`/`vldr` for VFP physical registers.
- Save/restore callee-saved VFP registers in prolog/epilog if any are used (usually `d8-d15` / `s16-s31`, but `fpv5-sp-d16` only has `s0-s15` caller-saved; confirm per AAPCS).

Key files:
- `libs/tinycc/ir/regalloc.c`
- `libs/tinycc/arm-thumb-gen.c` (spill emitter, prolog/epilog)

Tests:
- `tests/ir_tests/asm/fp_spill_pressure.c`: a function with many live `float` locals forcing spills; assert `vstr`/`vldr` and no helper calls.

### Phase 6 — Regression and integration

- Run `make ut`.
- Run `pytest tests/ir_tests/test_codegen_asm.py`.
- Run the QEMU smoke suite (`scripts/run_qemu_smoke.sh`) on FP-heavy cases.
- Run a self-host FAT-drive round-trip compiling tinycc itself with `-mfloat-abi=hard` once the basic cases pass.
- Regenerate `SOURCE_COVERAGE.md` if any newly-covered files change status.

## Key files / deliverables

Modify:
- `libs/tinycc/arm-thumb-gen.c` — VFP materialization, `tcc_gen_machine_fp_mop_hard()`, call/return layout, spills.
- `libs/tinycc/ir/regalloc.c` — confirm VFP register allocation honors `use_vfp`.
- `libs/tinycc/tests/ir_tests/test_codegen_asm.py` — new assertions for hard-float codegen.
- `libs/tinycc/docs/plan_whole_tinycc_coverage.md` — close the FP gap finding once fixed.

New test inputs:
- `tests/ir_tests/asm/fp_hard_basic.c`
- `tests/ir_tests/asm/fp_hard_ops.c`
- `tests/ir_tests/asm/call_fp_args.c`
- `tests/ir_tests/asm/call_fp_return.c`
- `tests/ir_tests/asm/fp_spill_pressure.c`

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Mixed int/FP AAPCS layout is subtle | Add dedicated tests with every permutation of GPR/VFP/stack args; compare with `arm-none-eabi-gcc -mfloat-abi=hard` disassembly for a reference corpus. |
| Soft-float regression | Keep the existing soft-float path untouched; gate every new branch on `float_abi == ARM_HARD_FLOAT`. Run the full QEMU `ir_tests` corpus with `-mfloat-abi=soft` before and after. |
| VFP register allocation bugs | Start with `-O0`/`-O1` only; the existing `use_vfp` flag already guides the RA. If RA mis-allocates, add targeted unit tests in `test_ra_*.c`. |
| Self-host miscompile | The cross compiler itself is built with soft-float, so this change only affects user code compiled with `-mfloat-abi=hard`. Still, run the FAT-drive self-host with a hard-float test subset. |
| Double-precision on `fpv5-sp-d16` | `fpv5-sp-d16` has no DP hardware, so `double` ops on that FPU must fall back to `__aeabi_d*` even under `-mfloat-abi=hard`. The plan honors `architecture_config.fpu->has_dadd` etc. |

## Stop criterion

`pytest tests/ir_tests/test_codegen_asm.py -k fp_hard` passes, `make ut` is green, and the QEMU smoke suite shows no new failures when run with both `-mfloat-abi=soft` and `-mfloat-abi=hard -mfpu=fpv5-sp-d16`.
