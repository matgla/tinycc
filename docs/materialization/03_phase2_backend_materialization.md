# Phase 2: Backend-Driven Materialization

> **Status: 🔄 Partial** — `tcc_gen_machine_data_processing_mop()` implemented for 11 data-processing ops (ADD, SUB, CMP, SHL, SHR, SAR, AND, OR, XOR, ADC_GEN, ADC_USE). `tcc_gen_machine_assign_mop()`, `tcc_gen_machine_setif_mop()`, and `tcc_gen_machine_bool_mop()` implemented. All other instruction types still use the old `tcc_gen_machine_*_op(IROperand)` path. `fill_registers_ir` continues to run unconditionally for all ops.

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
Condition: non-pair dest (`!irop_needs_pair`) and no static chain (`!ir->has_static_chain`)

The dispatch path in `ir/codegen.c` determines `use_mop_dp` **after** `fill_registers_ir` runs, then calls `machine_op_from_ir` on the already-filled operands. The `mach_*` helpers inside handle:
- `MACH_OP_REG` — value already in register, use directly
- `MACH_OP_SPILL` — load to scratch via `get_scratch_reg_with_save` + `load_to_reg_ir`
- `MACH_OP_IMM` — check if Thumb-encodable; if not, load to scratch
- `MACH_OP_FRAME_ADDR` — compute FP + offset into scratch

### `tcc_gen_machine_assign_mop()` — **DONE**

Handles: TCCIR_OP_ASSIGN (register moves, truncate, sign-extend)
Condition: non-pair, no static chain, **REG-only destination** (`mop_dest.kind == MACH_OP_REG && !mop_dest.needs_deref`)

The REG-only destination restriction exists because `mach_writeback_dest` → `tcc_machine_store_spill_slot` calls `fp_adjust_local_offset(offset, 0)` (always `is_param=0`), while the old `assign_op` → `store_ex_ir` → `th_store_resolve_base_ir` correctly uses `fp_adjust_local_offset(offset, sv.is_param)`. Spill/param destinations fall back to the old `assign_op`.

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
Condition: non-pair, no static chain

BOOL_OR: `mach_ensure_in_reg` for both sources, ORRS into dest, then IT NE / MOV #1 / IT EQ / MOV #0.
BOOL_AND: CMP src1, #0 / IT EQ / MOV dest, #0 / CMP src2, #0 / IT EQ / MOV dest, #0 / ... (short-circuit pattern).

### `MachineCodegenContext` — **NOT YET IMPLEMENTED**

The context struct described in Step 2.1 was not needed for the data-processing ops because `arm-thumb-gen.c` uses global state (`g_insn_scratch_count`, `g_insn_scratch_saves`) for per-instruction scratch bookkeeping. If more complex handlers require per-instruction context passing, this may be added then.

## Remaining Conversion Work

**Conversion order (easiest to hardest):**

1. ~~`tcc_gen_machine_data_processing_op` — ADD/SUB/CMP/SHL/SHR/SAR/AND/OR/XOR/ADC~~ ✅ Done
2. ~~`tcc_gen_machine_assign_op` — register moves / truncate / sign-extend~~ ✅ Done (REG-only dest)
3. ~~`tcc_gen_machine_bool_op` / `tcc_gen_machine_setif_op` — boolean and conditional set~~ ✅ Done
4. `tcc_gen_machine_load_op` / `tcc_gen_machine_store_op` — memory access
5. `tcc_gen_machine_lea_op` — address computation
6. `tcc_gen_machine_return_value_op` — function return
7. `tcc_gen_machine_jump_op` / `_conditional_jump_op` — control flow
8. `tcc_gen_machine_func_call_op` — function calls (most complex)
9. `tcc_gen_machine_func_parameter_op` — parameter passing
10. `tcc_gen_machine_fp_op` — floating point
11. `tcc_gen_machine_load_indexed_op` / `_store_indexed_op` — indexed memory
12. `tcc_gen_machine_load_postinc_op` / `_store_postinc_op` — post-increment
13. `tcc_gen_machine_vla_op` — VLA operations

For each handler: write `_mop` variant, update `ir/codegen.c` to call it (with `use_mop_*` flag), run tests, then delete old `_op` variant once all callers converted.

Once ALL handlers are on the MOP path, `fill_registers_ir` can be deleted and the dispatch loop reduces to raw operand → `machine_op_from_ir` → dispatch.

## Verification Checklist

- [x] `tcc_gen_machine_data_processing_mop()` implemented
- [x] `mach_ensure_in_reg()` / `mach_ensure_in_reg_or_imm()` / `mach_get_dest_reg()` / `mach_writeback_dest()` helpers implemented
- [x] `make test -j16` passes with data-processing on MOP path
- [x] ASSIGN/BOOL/SETIF ops on MOP path (ASSIGN restricted to REG-only dest)
- [ ] LOAD/STORE ops on MOP path
- [ ] LEA, RETURN, JUMP ops on MOP path
- [ ] Function call / parameter ops on MOP path
- [ ] FP ops on MOP path
- [ ] Indexed and post-increment memory ops on MOP path
- [ ] `fill_registers_ir` removed from dispatch loop (all handlers on MOP path)
- [ ] `tcc_ir_fill_registers_ir()` function deleted from `ir/codegen.c`
- [ ] `make test-gcc-torture-compile` passes
