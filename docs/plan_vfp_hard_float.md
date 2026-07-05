# Plan: Add ARMv8-M hard-float VFP support (`-mfloat-abi=hard`)

## Context

The YasOS TinyCC fork already parses `-mfloat-abi=hard` and `-mfpu=…`, sets `TCCState::float_abi` / `fpu_type`, and even configures `architecture_config.fpu` and VFP register allocation in `arm_init()`.  The VFP Thumb encoder (`arch/arm/thumb/thop_vfp.c`) is complete for the operations we need.

What is missing is the **codegen path**: `tcc_gen_machine_fp_mop()` in `arm-thumb-gen.c` unconditionally lowers every FP IR operation (`FADD`, `FSUB`, `FMUL`, `FDIV`, `FCMP`, `FNEG`, `CVT_ITOF`, `CVT_FTOI`, `CVT_FTOF`) to soft-float `__aeabi_*` library calls.  As a result, `fp_select.c` compiled with `-mfloat-abi=hard -mfpu=fpv5-sp-d16` still calls `__aeabi_fadd`/`__aeabi_dadd`/`__aeabi_fmul` and passes floats in integer registers.

The goal is to make `-mfloat-abi=hard` emit VFP instructions and use the VFP register bank for FP values, parameters, and return values, while keeping soft-float behavior unchanged.

## Current state summary

| Layer | State |
|---|---|
| Command-line parsing | `-mfloat-abi=hard` and `-mfpu=fpv{4,5}*dp{16,32}` parsed into `float_abi` / `fpu_type` |
| Feature resolution | `thumb_resolve_features()` in `arch/arm/thumb/thumb.c` maps `-mfpu=…` to `vfp_sp` / `vfp_dp` / `fp_armv8` bits |
| VFP encoder | `thop_vfp.c` has `th_vadd_f`, `th_vsub_f`, `th_vmul_f`, `th_vdiv_f`, `th_vcmp_f`, `th_vneg_f`, `th_vcvt_*`, `th_vmov_*`, `th_vpush`/`th_vpop`, `th_vmrs` |
| Allocator hint | `ir/vreg.c` sets `interval->use_vfp = (float_abi == ARM_HARD_FLOAT)` |
| FPU config | `arm_determine_fpu_config()` and `architecture_config.fpu` configured in `arm_init()` |
| Register bank | `s->float_registers_for_allocator` set to FPU register count when hard-float |
| **Missing** | Backend `tcc_gen_machine_fp_mop()` has no hard-float branch |
| **Missing** | AAPCS call layout (`thumb_build_call_layout_from_ir`) does not place FP args in `s0-s15`/`d0-d7` for hard-float |
| **Missing** | Return-value path does not use `s0`/`d0` for hard-float |

## Goal

When `float_abi == ARM_HARD_FLOAT` and the selected FPU supports the operation:

1. FP values live in VFP registers (`s0-s15` for single, `d0-d7` for double on `fpv5-d16`).
2. FP arithmetic/compare/negate/conversion lower to VFP instructions instead of `__aeabi_*` calls.
3. FP function arguments and return values follow the AAPCS hard-float convention (`s0-s15` / `d0-d7`, then stack).
4. Spills, reloads, and moves between GPR and VFP registers use `vldr`/`vstr`/`vmov`.
5. Existing soft-float (`-mfloat-abi=soft` / `softfp`) output is byte-for-byte unchanged.

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
