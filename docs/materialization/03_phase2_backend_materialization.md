# Phase 2: Backend-Driven Materialization

> **Status: ✅ Complete** — All convertible ops now have MOP handlers. Done: DP (ADD/SUB/CMP/SHL/SHR/SAR/AND/OR/XOR/ADC), ASSIGN (all dests), SETIF (including 64-bit pair dest), BOOL_OR/AND (including 64-bit pair sources), LOAD (including 64-bit pair), STORE, LOAD_INDEXED, STORE_INDEXED, LOAD_POSTINC, STORE_POSTINC, IJUMP, FUNCPARAMVAL/VOID, RETURNVALUE (32-bit and 64-bit), MUL/DIV group (MUL/DIV/UDIV/IMOD/UMOD/TEST_ZERO 32-bit; MLA/UMULL converted to dedicated MOP handlers), FP single-precision (FADD/FSUB/FMUL/FDIV/FNEG/FCMP/CVT_ITOF/CVT_FTOI/CVT_FTOF; doubles/complex stay on old path), VLA (VLA_ALLOC/VLA_SP_SAVE/VLA_SP_RESTORE), FUNC_CALL (32-bit and 64-bit non-complex dest; complex/static-chain stays on old path), SWITCH_TABLE. `!irop_needs_pair` guards removed for DP, ASSIGN, BOOL, LOAD, and FUNC_CALL — 64-bit pair sources handled via `mach_resolve_deref_64`. Three backend bugs fixed: (1) 64-bit reg-to-reg LOAD only copied lo half — added hi-half MOV; (2) dest/scratch register overlap in `dp_mop64`/`shift64_mop` — determine dest pair BEFORE deref resolution + pre-exclude src reg operands; (3) `MACH_OP_PARAM_STACK` double-indirection — added early return with `needs_deref=false`. JUMP/JUMPIF and LEA are intentionally left on the old path (see below).

## Goal

Move all materialization decisions into `arm-thumb-gen.c` instruction handlers, replacing the centralized `ir/codegen.c` materialize-then-dispatch pattern with per-instruction backend-driven materialization using `MachineOperand`.

## Current State (Actual Architecture)

The plan's original pseudocode was inaccurate. Here's what actually happens:

### Actual current flow

```
ir/codegen.c::tcc_ir_codegen_generate():
  1. Classify operand needs (need_src1_value, need_src2_value, ...)
  2. Get IROperand copies from pool
  3. Call tcc_ir_fill_registers_ir() on each operand
  4. Call tcc_ir_materialize_value_ir() / _addr_ir() / _dest_ir() as needed
  5. Call tcc_gen_machine_*_op() in arm-thumb-gen.c (which receives already-filled IROperands)
  6. Release scratch registers from materialization
```

### What arm-thumb-gen.c actually does

`arm-thumb-gen.c` does **NOT** call `tcc_ir_materialize_*` or `tcc_ir_mat_*` APIs. Instead it receives the pre-filled IROperands and then:

1. Calls `get_scratch_reg_with_save(exclude_mask)` — **66 times** across the file
2. Calls `load_to_reg_ir(reg, r1, src_operand)` — **63 times** across the file
3. Emits Thumb-2 instructions via `ot(th_xxx(...))`
4. Calls `restore_scratch_reg(&alloc)` to clean up

So there are **two layers of materialization**: `ir/mat.c` materializes into the IROperand, then `arm-thumb-gen.c` does its own `load_to_reg_ir` on top. This is the core redundancy.

## Proposed Pattern

Replace the current two-layer flow with a single-layer `MachineOperand`-based pattern:

### New `mach_*` helper functions (in `arm-thumb-gen.c`)

| Function | Role |
|---|---|
| `mach_ensure_in_reg(ctx, op)` | If REG: return reg. If SPILL: load to scratch. If IMM: mov to scratch. If FRAME_ADDR: compute address. |
| `mach_ensure_in_reg_or_imm(ctx, op)` | For ADD/SUB/CMP: return reg or encodable Thumb immediate |
| `mach_get_dest_reg(ctx, op)` | If dest is REG: return reg. If SPILL: allocate scratch. |
| `mach_writeback_dest(ctx, op, reg)` | If dest was SPILL: STR reg to spill slot. |
| `mach_ensure_addr(ctx, op)` | For LOAD/STORE: compute base register + offset. |
| `mach_release_scratch(ctx)` | Free scratch registers used in this instruction. |

### Example: TCCIR_OP_ADD — before and after

**Before (current):**
```c
// ir/codegen.c:
tcc_ir_fill_registers_ir(ir, &src1_ir);
tcc_ir_fill_registers_ir(ir, &src2_ir);
tcc_ir_fill_registers_ir(ir, &dest_ir);
tcc_ir_materialize_value_ir(ir, &src1_ir, &mat_src1);
tcc_ir_materialize_value_ir(ir, &src2_ir, &mat_src2);
tcc_ir_materialize_dest_ir(ir, &dest_ir, &mat_dest);
// Dispatch to backend:
tcc_gen_machine_data_processing_op(src1_ir, src2_ir, dest_ir, TCCIR_OP_ADD);
// arm-thumb-gen.c::tcc_gen_machine_data_processing_op():
//   calls get_scratch_reg_with_save() and load_to_reg_ir() again!
// ir/codegen.c:
tcc_machine_release_scratch(&mat_src1.scratch); // etc.
```

**After (proposed):**
```c
// ir/codegen.c (thin):
MachineOperand src1 = machine_op_from_ir(ir, &raw_src1);
MachineOperand src2 = machine_op_from_ir(ir, &raw_src2);
MachineOperand dest = machine_op_from_ir(ir, &raw_dest);
// Dispatch to backend:
tcc_gen_machine_data_processing_mop(ctx, src1, src2, dest, TCCIR_OP_ADD);

// arm-thumb-gen.c::tcc_gen_machine_data_processing_mop():
int r_src1 = mach_ensure_in_reg(ctx, &src1);
int r_src2 = mach_ensure_in_reg_or_imm(ctx, &src2, &is_imm, &imm_val);
int r_dest = mach_get_dest_reg(ctx, &dest);

if (is_imm)
    ot(th_add_imm(r_dest, r_src1, imm_val));
else
    ot(th_add_reg(r_dest, r_src1, r_src2));

mach_writeback_dest(ctx, &dest, r_dest);
mach_release_scratch(ctx);
```

## Implementation Steps

### Step 2.1: Define `MachineCodegenContext`

**Action:** Add a context struct to hold per-instruction state:

```c
typedef struct {
    TCCIRState *ir;
    int instruction_index;

    /* Scratch register pool for current instruction */
    int scratch_regs[4];
    int scratch_count;
    int scratch_used;

    /* Track which physical registers are live at this point */
    uint16_t live_reg_mask;

    /* Plan mode (dry run) vs emit mode */
    bool plan_mode;
} MachineCodegenContext;
```

**File:** `arm-thumb-gen.c` (or a new `arm-thumb-mach.h` header)

### Step 2.2: Implement `mach_ensure_in_reg()`

**Action:** This wraps the existing `get_scratch_reg_with_save` + `load_to_reg_ir` pattern:

```c
static int mach_ensure_in_reg(MachineCodegenContext *ctx, const MachineOperand *op)
{
    switch (op->kind) {
    case MACH_OP_REG:
        return op->u.reg.r0;

    case MACH_OP_SPILL: {
        int scratch = mach_alloc_scratch(ctx, /* exclude= */ 0);
        int offset = op->u.spill.offset;
        // LDR scratch, [fp, #offset]
        emit_ldr_spill(scratch, offset, op->u.spill.size);
        if (op->needs_deref) {
            // Double indirection: load pointer, then load through it
            emit_ldr_indirect(scratch, scratch, 0, /* size from type */);
        }
        return scratch;
    }

    case MACH_OP_IMM: {
        int scratch = mach_alloc_scratch(ctx, 0);
        emit_mov_imm(scratch, op->u.imm.val);
        return scratch;
    }

    case MACH_OP_FRAME_ADDR: {
        int scratch = mach_alloc_scratch(ctx, 0);
        emit_add_fp_offset(scratch, op->u.frame.offset);
        return scratch;
    }

    case MACH_OP_SYMBOL: {
        int scratch = mach_alloc_scratch(ctx, 0);
        emit_load_symbol_addr(scratch, op->u.sym.sym, op->u.sym.addend);
        return scratch;
    }

    case MACH_OP_PARAM_STACK: {
        int scratch = mach_alloc_scratch(ctx, 0);
        emit_ldr_param(scratch, op->u.param.offset, op->u.param.size);
        return scratch;
    }
    }
}
```

**Key insight:** Each `case` here corresponds to what `ir/mat.c` currently tests with multiple flag combinations. The explicit `kind` enum makes the code self-documenting.

### Step 2.3: Implement remaining `mach_*` helpers

Implement in `arm-thumb-gen.c`:

- `mach_ensure_in_reg_or_imm(ctx, op, &is_imm, &imm_val)` — checks if IMM value is Thumb-encodable; if so, returns the immediate; otherwise loads to scratch register.
- `mach_get_dest_reg(ctx, op)` — returns physical reg or allocates scratch for spilled dest.
- `mach_writeback_dest(ctx, op, reg)` — STR to spill slot if dest was spilled.
- `mach_ensure_addr(ctx, op)` — for LOAD/STORE, returns base register + offset pair.
- `mach_alloc_scratch(ctx, exclude_mask)` — wraps `get_scratch_reg_with_save()`.
- `mach_release_scratch(ctx)` — wraps `restore_scratch_reg()`.

### Step 2.4: Convert instruction handlers one-by-one

**Action:** Create `_mop` variants of each `tcc_gen_machine_*_op` function that accept `MachineOperand` instead of `IROperand`. Start with the simplest:

**Conversion order (easiest to hardest):**

1. `tcc_gen_machine_data_processing_op` — arithmetic (ADD, SUB, MUL, etc.)
2. `tcc_gen_machine_load_op` / `tcc_gen_machine_store_op` — memory access
3. `tcc_gen_machine_assign_op` — register moves
4. `tcc_gen_machine_return_value_op` — function return
5. `tcc_gen_machine_lea_op` — address computation
6. `tcc_gen_machine_jump_op` / `_conditional_jump_op` — control flow
7. `tcc_gen_machine_setif_op` — conditional set
8. `tcc_gen_machine_bool_op` — boolean ops
9. `tcc_gen_machine_func_call_op` — function calls (most complex)
10. `tcc_gen_machine_func_parameter_op` — parameter passing
11. `tcc_gen_machine_fp_op` — floating point
12. `tcc_gen_machine_load_indexed_op` / `_store_indexed_op` — indexed memory
13. `tcc_gen_machine_load_postinc_op` / `_store_postinc_op` — post-increment
14. `tcc_gen_machine_vla_op` — VLA operations

**For each handler:**
1. Write `_mop` version alongside existing `_op` version
2. Update `ir/codegen.c` dispatch to call `_mop` version (passing `MachineOperand` instead of `IROperand`)
3. Run `make test -j16`
4. Once all callers converted, delete the old `_op` version

### Step 2.5: Update `ir/codegen.c` dispatch loop

**Action:** Replace the centralized materialize-then-dispatch pattern:

```c
// BEFORE (current):
tcc_ir_fill_registers_ir(ir, &src1_ir);
tcc_ir_materialize_value_ir(ir, &src1_ir, &mat_src1);
// ... then dispatch, then release

// AFTER:
MachineOperand src1 = machine_op_from_ir(ir, &raw_src1);
// ... then dispatch (handler does its own materialization)
```

The dispatch loop becomes ~50% shorter because the classify-materialize-release boilerplate is deleted.

### Step 2.6: Handle 64-bit values

**Special attention:** 64-bit values (long long, double) use register pairs. The `mach_ensure_in_reg()` function must return both registers:

```c
typedef struct {
    int r0;
    int r1;  /* -1 if not 64-bit */
} MachRegPair;

MachRegPair mach_ensure_in_reg_pair(MachineCodegenContext *ctx, const MachineOperand *op);
```

For spilled 64-bit values, this loads two words from adjacent spill slots. For register pairs, it returns both physical regs.

## What Is Actually Implemented

### `tcc_gen_machine_data_processing_mop()` — **DONE**

Handles: ADD, SUB, CMP, SHL, SHR, SAR, AND, OR, XOR, ADC_GEN, ADC_USE
Condition: no static chain (`!ir->has_static_chain`); `!irop_needs_pair` guard has been removed — 64-bit pair sources are now handled via `mach_resolve_deref_64`

The dispatch path in `ir/codegen.c` determines `use_mop_dp` **after** `fill_registers_ir` runs, then calls `machine_op_from_ir` on the already-filled operands. The `mach_*` helpers inside handle:
- `MACH_OP_REG` — value already in register, use directly
- `MACH_OP_SPILL` — load to scratch via `get_scratch_reg_with_save` + `load_to_reg_ir`
- `MACH_OP_IMM` — check if Thumb-encodable; if not, load to scratch
- `MACH_OP_FRAME_ADDR` — compute FP + offset into scratch

### `tcc_gen_machine_assign_mop()` — **DONE**

Handles: TCCIR_OP_ASSIGN (register moves, truncate, sign-extend)
Condition: no static chain (`!ir->has_static_chain`); `!irop_needs_pair` guard has been removed — 64-bit pair sources/destinations are handled via `mach_resolve_deref_64` and the existing 64-bit assign path in `tcc_gen_machine_assign_mop`

All destination kinds supported: REG (direct), SPILL (via `mach_get_dest_reg` scratch + `mach_writeback_dest` → `tcc_machine_store_spill_slot`), PARAM_STACK (via `mach_writeback_dest` → `tcc_machine_store_param_slot`). The earlier REG-only restriction has been removed — `tcc_machine_store_spill_slot` correctly applies `fp_adjust_local_offset`, which was the original concern.

Source operand handling covers all `MachineOperandKind` variants:
- `MACH_OP_REG` (no deref) → direct `mach_writeback_dest` (0 scratch)
- `MACH_OP_REG` (deref) → `load_from_base_ir` into dest_reg
- `MACH_OP_IMM` → `tcc_machine_load_constant` into dest_reg
- `MACH_OP_SPILL` → `tcc_machine_load_spill_slot` + optional deref
- `MACH_OP_SYMBOL` → `tcc_machine_load_constant` with sym + optional deref
- `MACH_OP_FRAME_ADDR` → `tcc_machine_addr_of_stack_slot`
- `MACH_OP_PARAM_STACK` → `load_from_base_ir` with `offset_to_args` adjustment

A special `assign_before_ret` guard in both dry-run and real-run prevents the ASSIGN MOP path from firing when the next instruction is RETURNVALUE (to preserve the existing RETURNVALUE peephole that sets `dest_ir.pr0_reg = REG_IRET`). The guard also checks `!has_incoming_jump[i+1]` to ensure consistency between dry-run and real-run.

### `tcc_gen_machine_setif_mop()` — **DONE**

Handles: TCCIR_OP_SETIF (conditional set)
Condition: non-pair, no static chain

Emits: MOV dest, #0; IT cond; MOV dest, #1. Uses `mach_get_dest_reg` / `mach_writeback_dest` for destination, no source operand materialization needed (reads from condition flags).

### `tcc_gen_machine_bool_mop()` — **DONE**

Handles: TCCIR_OP_BOOL_OR, TCCIR_OP_BOOL_AND
Condition: no static chain (`!ir->has_static_chain`); `!irop_needs_pair` guard has been removed — 64-bit pair sources are now handled

BOOL_OR: `mach_ensure_in_reg` for both sources, ORRS into dest, then IT NE / MOV #1 / IT EQ / MOV #0.
BOOL_AND: CMP src1, #0 / IT EQ / MOV dest, #0 / CMP src2, #0 / IT EQ / MOV dest, #0 / ... (short-circuit pattern).

For 64-bit sources: lo and hi halves are ORR'd together to produce a single 32-bit "nonzero" value before the boolean operation.

### `tcc_gen_machine_func_call_mop()` — **DONE**

Handles: TCCIR_OP_FUNCCALLVAL, TCCIR_OP_FUNCCALLVOID
Condition: not complex (`!dest_ir.is_complex`), no static chain; `!irop_needs_pair(dest_ir)` guard has been removed — 64-bit pair destinations are now handled

The destination return value is a `MachineOperand dest_mop`, produced by `machine_op_from_ir(ir, &dest_ir)` in the dispatch loop. Internally, `handle_return_value_mop(&dest_mop, drop_value)` calls `mach_writeback_dest(&dest_mop, ARM_R0)`, which handles:
- `MACH_OP_REG` — emit `MOV dest.r0, R0` when `r0 != ARM_R0`; for 64-bit: also `MOV dest.r1, R1`
- `MACH_OP_SPILL` — call `tcc_machine_store_spill_slot(R0, offset)`; for 64-bit: also store R1 at offset+4
- `MACH_OP_NONE` — no-op (void or drop_value)

`func_target` and `call_id_op` were **converted to MachineOperand** in Phase 5g:
- `gcall_or_jump_mop()` replaces `gcall_or_jump_ir()`, taking `MachineOperand func_mop` instead of reading `func_target.pr0_reg`
- Pre-save logic rewritten to use `func_mop.kind`, `func_mop.u.reg.r0`, `func_mop.needs_deref`
- `thumb_build_call_layout_from_ir()` extended with `MachineOperand **out_mops` parameter (Phase 5k)

**Architecture note:** `tcc_gen_machine_func_call_op()` was deleted in Phase 5j. All function call codegen now goes through `tcc_gen_machine_func_call_mop()`, which handles all cases including complex types and static-chain functions (via `MACH_OP_CHAIN_REL`). `handle_return_value_mop` handles both 32-bit and 64-bit dest pairs (R0+R1 writeback).

### `mach_resolve_deref_64()` — **DONE**

Helper added to handle `needs_deref` 64-bit source operands before lo/hi half splitting. When a source `MachineOperand` has `needs_deref=true` and `is_64bit=true`, calling `mach_make_lo_half`/`mach_make_hi_half` directly is incorrect: `mach_make_hi_half` increments the register number (R0→R1) instead of the memory offset (+4), producing bogus loads.

`mach_resolve_deref_64` resolves this by:
1. If `!needs_deref`: returns `*op` unchanged.
2. **PARAM_STACK special case:** If `op->kind == MACH_OP_PARAM_STACK`, returns `*op` with `needs_deref=false` (for stack params, `needs_deref=true` means "value IS at this stack slot," not "pointer at this slot to follow" — treating it as double indirection was **Bug #3**, fixed here).
3. Strips `needs_deref`, gets base address register via `mach_ensure_in_reg`.
4. Allocates two scratch registers.
5. Loads `[base+0]` → lo_reg and `[base+4]` → hi_reg via `load_from_base_ir(..., IROP_BTYPE_INT32, ...)`.
6. Returns a clean `MACH_OP_REG` pair operand with `is_64bit=true`, `needs_deref=false`.

Called at entry of `thumb_emit_data_processing_mop64` (for both src1 and src2) and `thumb_emit_shift64_mop` (for src1) before any lo/hi splitting.

**Bug #2 fix — Dest/scratch register overlap:** `mach_resolve_deref_64` allocates scratch registers, which could overlap with the dest register pair when dest was determined AFTER deref resolution. Fixed by:
- (a) Determining dest register pair (via `mach_get_dest_reg_pair`) BEFORE calling `mach_resolve_deref_64`.
- (b) Pre-excluding src1/src2 register operands from the scratch pool BEFORE deref resolution (preventing scratch from overlapping src registers that haven't been loaded yet).

**Bug #3 fix — PARAM_STACK deref:** For `MACH_OP_PARAM_STACK`, `needs_deref=true` signals "value is at this stack offset" (ARM AAPCS: 64-bit params passed at aligned stack slots for args beyond r0–r3). The deref helper was loading the 64-bit value from the stack slot, then treating that as a pointer and loading through it — double indirection. Fixed by returning early with `needs_deref=false`.

### `MachineCodegenContext` — **NOT YET IMPLEMENTED**

The context struct described in Step 2.1 was not needed for the data-processing ops because `arm-thumb-gen.c` uses global state (`g_insn_scratch_count`, `g_insn_scratch_saves`) for per-instruction scratch bookkeeping. If more complex handlers require per-instruction context passing, this may be added then.

## Remaining Conversion Work

**Conversion order (easiest to hardest):**

1. ~~`tcc_gen_machine_data_processing_op` — ADD/SUB/CMP/SHL/SHR/SAR/AND/OR/XOR/ADC~~ ✅ Done
2. ~~`tcc_gen_machine_assign_op` — register moves / truncate / sign-extend (all dests)~~ ✅ Done
3. ~~`tcc_gen_machine_bool_op` / `tcc_gen_machine_setif_op` — boolean and conditional set~~ ✅ Done
4. ~~`tcc_gen_machine_load_op` / `tcc_gen_machine_store_op` — memory access~~ ✅ Done
5. ~~`tcc_gen_machine_load_indexed_op` / `_store_indexed_op` — indexed memory~~ ✅ Done
6. ~~`tcc_gen_machine_load_postinc_op` / `_store_postinc_op` — post-increment~~ ✅ Done
7. ~~`tcc_gen_machine_indirect_jump_op` (IJUMP)~~ ✅ Done
8. ~~`tcc_gen_machine_func_parameter_op` (FUNCPARAMVAL/VOID)~~ ✅ Done
9. ~~`tcc_gen_machine_return_value_op` — function return (32-bit only; 64-bit stays on old path)~~ ✅ Done
10. ~~`tcc_gen_machine_data_processing_op` — MUL/DIV/UDIV/IMOD/UMOD/TEST_ZERO (32-bit; MLA/UMULL stay on old path)~~ ✅ Done
11. `tcc_gen_machine_lea_op` — **SKIP**: already handles spilled dest internally; no double-materialization; chain-tracking adds non-trivial complexity for no phase-3 benefit
12. `tcc_gen_machine_jump_op` / `_conditional_jump_op` — **SKIP**: no register materialization at all (reads `src.u.imm32` / `dest.u.imm32` directly); MOP wrapper would add zero value
13. ~~`tcc_gen_machine_func_call_op` — function calls~~ ✅ Done
    - `tcc_gen_machine_func_call_mop()` handles 32-bit and 64-bit non-complex dest via `MachineOperand dest_mop`.
    - `tcc_gen_machine_func_call_op()` retains its full implementation for the old path (complex, static chain). **Not a wrapper** — `handle_return_value()` (legacy with SValue compat) is only in `_op`; `handle_return_value_mop()` (32-bit and 64-bit via `MachineOperand`) is in `_mop`.
    - `func_target` and `call_id_op` converted to MachineOperand (Phase 5g); callsite uses `MachineOperand **out_mops` (Phase 5k).
14. ~~`tcc_gen_machine_fp_op` — floating point (single-precision; doubles/complex stay on old path)~~ ✅ Done
15. ~~`tcc_gen_machine_vla_op` — VLA operations~~ ✅ Done

For each handler: write `_mop` variant, update `ir/codegen.c` to call it (with `use_mop_*` flag), run tests, then delete old `_op` variant once all callers converted.

Once ALL handlers are on the MOP path, `fill_registers_ir` can be deleted and the dispatch loop reduces to raw operand → `machine_op_from_ir` → dispatch.

## Verification Checklist

- [x] `tcc_gen_machine_data_processing_mop()` implemented
- [x] `mach_ensure_in_reg()` / `mach_ensure_in_reg_or_imm()` / `mach_get_dest_reg()` / `mach_writeback_dest()` helpers implemented
- [x] `make test -j16` passes with data-processing on MOP path
- [x] ASSIGN MOP (all dests), BOOL, SETIF ops on MOP path
- [x] LOAD / STORE ops on MOP path
- [x] LOAD_INDEXED / STORE_INDEXED / LOAD_POSTINC / STORE_POSTINC ops on MOP path
- [x] IJUMP (indirect jump) on MOP path
- [x] FUNCPARAMVAL / FUNCPARAMVOID on MOP path
- [x] RETURNVALUE on MOP path (32-bit; 64-bit/static-chain stays on old path)
- [x] MUL/DIV group on MOP path (MUL/DIV/UDIV/IMOD/UMOD/TEST_ZERO 32-bit; MLA/UMULL stay on old path)
- [N/A] LEA — skipped (single-layer already, handles spilled dest, chain-tracking complexity)
- [N/A] JUMP / JUMPIF — skipped (no register materialization, no scratch allocation)
- [x] FP single-precision on MOP path (FADD/FSUB/FMUL/FDIV/FNEG/FCMP/CVT_ITOF/CVT_FTOI/CVT_FTOF; doubles/complex stay on old path)
- [x] VLA on MOP path (VLA_ALLOC/VLA_SP_SAVE/VLA_SP_RESTORE)
- [x] FUNCCALLVAL / FUNCCALLVOID on MOP path (32-bit non-pair dest; dest replaced by `MachineOperand dest_mop`;
      `func_target` and `call_id_op` still passed as filled IROperands; 64-bit/complex/static-chain stays on old path)
- [x] `irop_needs_pair` guards removed for DP and ASSIGN — 64-bit pair sources handled via `mach_resolve_deref_64`
      (loads `[base+0]` / `[base+4]` into scratch regs before lo/hi splitting; applied in `thumb_emit_data_processing_mop64`
      for both src1/src2 and `thumb_emit_shift64_mop` for src1)
- [x] `irop_needs_pair` guards removed for BOOL — 64-bit pair sources handled via lo/hi ORR reduction
- [x] `irop_needs_pair` guards removed for LOAD — 64-bit pair sources handled (including reg-to-reg hi-half MOV fix)
- [x] `irop_needs_pair` guards removed for FUNC_CALL dest — 64-bit pair return values handled via `handle_return_value_mop`
      (R0 + R1 writeback to dest pair); `is_complex` guard retained
- [x] Bug fix: 64-bit reg-to-reg LOAD — `tcc_gen_machine_load_mop` MACH_OP_REG non-deref case added hi-half MOV
      (`src.u.reg.r1 → dest_r1`) for 64-bit register pairs
- [x] Bug fix: dest/scratch overlap in `thumb_emit_data_processing_mop64` and `thumb_emit_shift64_mop` — moved dest
      register pair determination BEFORE `mach_resolve_deref_64` calls; added pre-exclusion of src1/src2 register
      operands from scratch pool
- [x] Bug fix: PARAM_STACK double-indirection in `mach_resolve_deref_64` — added early return for
      `MACH_OP_PARAM_STACK` with `needs_deref=false` (value IS at stack slot, not pointer to follow)
- [x] `handle_return_value_mop` supports 64-bit dest — writes R0→dest.r0 and R1→dest.r1 (or spills both)
- [x] `tcc_gen_machine_bool_mop` supports 64-bit sources — lo/hi halves ORR'd to single nonzero test
- [x] 32-bit lvalue→64-bit dest ASSIGN bug fixed — `if (src.needs_deref)` changed to `if (src.needs_deref && src.is_64bit)`
      in `tcc_gen_machine_assign_mop`: when a stack parameter is a 32-bit pointer that is being widened into a 64-bit dest
      register pair, `needs_deref=true` but `is_64bit=false`; without the guard this incorrectly loaded `[ptr+0]`/`[ptr+4]`
      (dereferencing 64-bit content through the pointer) instead of zero-extending the pointer value itself
- [x] `fill_registers_ir` removed from dispatch loop — ✅ done (Phase 5b removed dispatch-level fills;
      Phase 5f rewrote `machine_op_from_ir` to read interval table directly; Phase 5m deleted `fill_registers_ir`)
- [x] `tcc_ir_fill_registers_ir()` function deleted from `ir/codegen.c` — ✅ done (Phase 5m)
