# SSA-Based Register Allocator — Implementation Plan

## Context

Step 4 of `plan_ssa.md`: replace `tcc_ir_liveness_analysis()` + `tcc_ls_allocate_registers()` with a clean SSA-aware register allocator. The current allocator (`tccls.c`) works on flat IR after SSA destruction. The new allocator operates directly on SSA-renamed IR with phi nodes — simpler liveness, no lossy SSA destruction, and cleanly separated from the old code.

## Pipeline

Current:
```
SSA construct → rename → destroy → optimize → liveness(ir/live.c) → allocate(tccls.c) → codegen
```

New (when `-fssa-regalloc` enabled):
```
[SKIP first SSA pass] → optimize → [build SSA] → SSA regalloc → codegen
```

Skip the first SSA pass when SSA regalloc is enabled. Optimizations work without it (they did before SSA was added). After optimization, VARs still have multi-defs, and the existing `ir/ssa.c` handles VARs natively.

When disabled: pipeline unchanged.

## File Layout — Arch-Independent vs Arch-Dependent

### Arch-independent: `ir/regalloc.c` + `ir/regalloc.h`

Core SSA register allocator — no ARM-specific knowledge:

- **SSA live interval building**: scan SSA instructions + phi nodes → `[start, end]` per vreg
- **Linear scan allocation**: sort intervals by start, sweep, assign from abstract register pools
- **Phi resolution**: sequentialize parallel copies, insert ASSIGN instructions
- **Instruction array rebuild**: fix jump targets, remap indices

The allocator receives register constraints through an abstract interface:

```c
/* Arch-independent register class descriptor */
typedef struct RegAllocClass {
    int num_regs;              /* total registers in class */
    const int *caller_saved;   /* caller-saved register list */
    int num_caller_saved;
    const int *callee_saved;   /* callee-saved register list */
    int num_callee_saved;
    int pair_align;            /* 1 = pairs must be even-aligned (AAPCS) */
} RegAllocClass;

/* Arch-independent allocation target */
typedef struct RegAllocTarget {
    RegAllocClass int_class;   /* integer registers */
    RegAllocClass fp_class;    /* float/VFP registers */
    int param_regs;            /* number of parameter registers (e.g. 4) */
    int static_chain_reg;      /* -1 if none */
} RegAllocTarget;
```

Entry point:
```c
void tcc_ir_ssa_regalloc(TCCIRState *ir, const RegAllocTarget *target, int spill_base);
```

### Arch-dependent: `arch/arm/arm_regalloc.c` + `arch/arm/arm_regalloc.h`

ARM-specific register set definitions:

```c
/* Provides the RegAllocTarget for ARM Thumb-2 */
const RegAllocTarget *arm_get_regalloc_target(void);
```

Contains:
- R0-R3 as caller-saved, R4-R11 as callee-saved (AAPCS)
- VFP register set (S0-S15 caller-saved)
- Even-aligned pair rule for 64-bit (R0:R1, R2:R3, etc.)
- Parameter register count (4)
- Static chain register (R10)

Small file (~50 lines) — just data tables, no algorithms.

## Algorithm Details

### SSA Live Interval Building

For each vreg in SSA-renamed IR, compute `[start, end]`:

1. **Scan instructions**: For each instruction `i`:
   - Each USE vreg: extend `end = max(end, i)`
   - Each DEF vreg: set `start = i` (single-def in SSA)

2. **Process phi nodes**: For each block `b`, for each phi:
   - `phi.dest_vreg`: set `start = b.start_idx`
   - For each operand `(vreg_k, pred_k)`: extend `vreg_k.end = pred_block.end_idx - 1`

3. **FUNCPARAMVAL chains**: Extend parameter vreg intervals from FUNCPARAMVAL to corresponding FUNCCALL

4. **Call crossings**: Build call-site prefix-sum array, check if interval spans any call

5. **PARAMs**: Start at instruction 0, precolored to parameter registers

6. **Address-taken VARs**: Not SSA-renamed; mark `addrtaken=1`, force stack

### Linear Scan Allocation

New implementation, independent of `tccls.c`:

1. Sort intervals by start point (params first for precoloring)
2. Sweep in order, maintain active set (sorted by end point):
   - Expire intervals ending before current start → free their registers
   - If address-taken: force spill to stack
   - If crosses call: prefer callee-saved register
   - If 64-bit: allocate aligned pair (from `RegAllocTarget` pair rules)
   - If float: allocate from float register class
   - If no register available: spill (evict interval with fewest uses / longest range)
3. Track dirty_registers bitmap for prologue/epilogue

Output: write directly to `IRLiveInterval.allocation` (r0, r1, offset) via `tcc_ir_stack_reg_assign()` — same output format consumed by `machine_op_from_ir()`.

### Phi Resolution (after allocation)

For each predecessor block, collect all phi copies `(dest_reg, src_reg)`:
1. Filter identity copies (dest == src)
2. Topological sort for dependency order
3. For cycles: break with scratch register or temp stack slot
4. Insert ASSIGN instructions before block terminator

### Instruction Array Rebuild

Same pattern as `tcc_ir_ssa_destroy()`:
1. Build `old_to_new[]` index mapping
2. Fix JUMP/JUMPIF targets, switch table targets, `is_jump_target` flags
3. Remap `IRLiveInterval.start/end`
4. Build `live_regs_by_instruction` table from final intervals

## Pipeline Integration (`tccgen.c`)

```c
/* SSA for optimizations — skip when SSA regalloc handles it later */
if (tcc_state->opt_ssa && !tcc_state->opt_ssa_regalloc) {
    /* existing: construct → rename → destroy */
}

/* ... optimizations as today ... */

/* Register allocation */
if (tcc_state->opt_ssa_regalloc) {
    const RegAllocTarget *target = arm_get_regalloc_target();
    tcc_ir_ssa_regalloc(ir, target, loc);
} else {
    tcc_ir_liveness_analysis(ir);
    tcc_ls_allocate_registers(&ir->ls, ...);
}

/* ... rest unchanged: move coalescing, patch, params, stack, codegen ... */
```

## Files to Create/Modify

| File | Change |
|------|--------|
| `ir/regalloc.c` | **NEW** — arch-independent SSA regalloc (~400 lines) |
| `ir/regalloc.h` | **NEW** — `RegAllocTarget`, `tcc_ir_ssa_regalloc()` |
| `arch/arm/arm_regalloc.c` | **NEW** — ARM register set tables (~50 lines) |
| `arch/arm/arm_regalloc.h` | **NEW** — `arm_get_regalloc_target()` |
| `ir/ir.h` | Add `#include "regalloc.h"` |
| `tccgen.c` | Route to SSA regalloc when flag enabled (~20 lines) |
| `tcc.h` | Add `opt_ssa_regalloc` field to `TCCState` (near line 1144) |
| `libtcc.c` | Add `"ssa-regalloc"` to `-f` flag table (near line 1738) |
| `Makefile` | Add `ir/regalloc.c` + `arch/arm/arm_regalloc.c` to build |

Files NOT modified: `tccls.c`, `ir/ssa.c`, `ir/cfg.c`, `ir/live.c`, `ir/codegen.c`, `arm-thumb-gen.c`, `ir/machine_op.c`

## Functions to Reuse (read-only)

- `tcc_ir_cfg_build()`, `tcc_ir_cfg_compute_dominators()`, `tcc_ir_cfg_compute_dom_frontiers()` — `ir/cfg.c`
- `tcc_ir_ssa_construct()`, `tcc_ir_ssa_rename()`, `tcc_ir_ssa_free()` — `ir/ssa.c`
- `tcc_ir_stack_reg_assign()` — `ir/stack.c` (writes `IRLiveInterval.allocation`)
- `tcc_ir_mark_return_value_incoming_regs()` — `ir/codegen.c`
- `tcc_ir_vreg_live_interval()` — `ir/vreg.c`
- `irop_config[]`, `tcc_ir_op_get_dest/src1/src2()`, `irop_get_vreg()` — `tccir_operand.h`

## Implementation Order

1. Create `arch/arm/arm_regalloc.h` + `arch/arm/arm_regalloc.c` — ARM register tables
2. Create `ir/regalloc.h` — `RegAllocTarget` structs + `tcc_ir_ssa_regalloc()` declaration
3. Create `ir/regalloc.c` — skeleton entry point, SSA build, live interval computation
4. Implement linear scan allocation (writes `IRLiveInterval.allocation` directly)
5. Implement phi resolution + instruction array rebuild
6. Wire into pipeline: `tccgen.c`, `tcc.h`, `libtcc.c`, `Makefile`, `ir/ir.h`
7. Test: `make test -j16`, `make test-gcc-torture-compile`

## Verification

```bash
make cross
# Test at -O0 with SSA regalloc
cd tests/ir_tests && python run.py -c 01_hello_world.c --cflags="-fssa-regalloc"
# Test at -O1
cd tests/ir_tests && python run.py -c 01_hello_world.c --cflags="-O1 -fssa-regalloc"
# Full suites
make test -j16
make test-gcc-torture-compile
```
