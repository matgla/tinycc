# Phase 4: Eliminate `ir/mat.c`

> **Status: ✅ COMPLETE** — committed `bc43b639 phase 4` + `0e772abb phase 5: remove dead files and dead TCCStackSlot fields`

## Goal

With all materialization handled by the backend (Phase 2), remove the IR-level materialization module entirely.

## Current State After Phase 2

At this point:
- All instruction handlers use `MachineOperand` + `mach_*` helpers
- `ir/codegen.c` dispatch loop only calls `machine_op_from_ir()`, no longer calls `tcc_ir_materialize_*_ir()`
- `ir/mat.c` functions are completely unused

## What Moves Where

| Current `ir/mat.c` function | Replacement |
|---|---|
| `tcc_ir_materialize_value_ir()` | `mach_ensure_in_reg()` in `arm-thumb-gen.c` |
| `tcc_ir_materialize_const_to_reg_ir()` | `mach_ensure_in_reg()` (IMM case) |
| `tcc_ir_materialize_addr_ir()` | `mach_ensure_addr()` in `arm-thumb-gen.c` |
| `tcc_ir_materialize_dest_ir()` | `mach_get_dest_reg()` in `arm-thumb-gen.c` |
| `tcc_ir_storeback_materialized_dest_ir()` | `mach_writeback_dest()` in `arm-thumb-gen.c` |
| `tcc_ir_release_materialized_*_ir()` | `mach_release_scratch()` in `arm-thumb-gen.c` |
| `tcc_ir_mat_spilled_op()` / `tcc_ir_is_spilled_ir()` | `machine_op.kind == MACH_OP_SPILL` |
| `tcc_ir_operand_needs_dereference()` | `machine_op.needs_deref` |

## What Stays in IR

| File | Status |
|---|---|
| `ir/live.c` | Unchanged — liveness analysis |
| `ir/vreg.c` | Unchanged — virtual register tracking |
| `ir/stack.c` | Simplified — only real locals + spill slots |
| `ir/codegen.c` | Reduced to `machine_op_from_ir()` conversion + dispatch loop |
| `ir/machine_op.h` | New — `MachineOperand` type (from Phase 1) |

## Implementation Steps

### Step 4.1: Verify no remaining callers of `ir/mat.c` functions

**Action:**
```bash
# These should all return 0 matches:
grep -rn 'tcc_ir_materialize_value_ir\b' --include='*.c' --include='*.h' | grep -v 'ir/mat.c'
grep -rn 'tcc_ir_materialize_const_to_reg_ir\b' --include='*.c' --include='*.h' | grep -v 'ir/mat.c'
grep -rn 'tcc_ir_materialize_addr_ir\b' --include='*.c' --include='*.h' | grep -v 'ir/mat.c'
grep -rn 'tcc_ir_materialize_dest_ir\b' --include='*.c' --include='*.h' | grep -v 'ir/mat.c'
grep -rn 'tcc_ir_storeback_materialized_dest_ir\b' --include='*.c' --include='*.h' | grep -v 'ir/mat.c'
grep -rn 'tcc_ir_release_materialized_.*_ir\b' --include='*.c' --include='*.h' | grep -v 'ir/mat.c'
grep -rn 'tcc_ir_mat_value\b\|tcc_ir_mat_const\b\|tcc_ir_mat_addr\b\|tcc_ir_mat_dest\b' --include='*.c' --include='*.h' | grep -v 'ir/mat.c'
```

If any callers remain, they must be converted to use `mach_*` helpers first.

### Step 4.2: Delete `ir/mat.c`

**Action:** Remove the entire file (~1096 lines).

### Step 4.3: Delete `ir/mat.h` (if it exists as a separate header)

**Action:** Remove materialization-related declarations. Check `tccir.h` for any remaining references:

- Remove `TCCMaterializedValue` struct
- Remove `TCCMaterializedAddr` struct
- Remove `TCCMaterializedDest` struct
- Remove `TCCMatValue` / `TCCMatAddr` / `TCCMatDest` wrapper types
- Remove function declarations for deleted functions

### Step 4.4: Remove `ir/mat.c` from build system

**Action:** Edit `Makefile` to remove `ir/mat.c` from source lists (look for `IR_SRC`, `TINYCC_IR_SRC`, or similar variables).

### Step 4.5: Reduce `ir/codegen.c`

**Action:** Remove now-dead code:

1. Delete `tcc_ir_fill_registers_ir()` (replaced by `machine_op_from_ir()`)
2. Delete the operand classification block (the `need_src1_value`, `need_src2_value`, etc. switch)
3. Delete the centralized materialization block
4. Delete the scratch release block at the end of the dispatch loop

The dispatch loop becomes:
```c
for each instruction:
    get raw operands from pool
    convert to MachineOperand via machine_op_from_ir()
    dispatch to tcc_gen_machine_*_mop() handler
    // (handler does its own materialization and cleanup)
```

**Expected:** `ir/codegen.c` reduces from ~2331 lines to ~400-600 lines.

### Step 4.6: Compile and test

```bash
make clean && make cross -j16
make test -j16
make test-gcc-torture-compile
```

## What Was Done

### Files deleted
- `ir/mat.c` — the entire IR-level materialization module (~1096 lines)
- `ir/operand.c` — IROperand utility functions that were part of the old materialization layer
- `ir/operand.h` — header for the above

### Replacement
- `ir/machine_op.c` + `ir/machine_op.h` — the new `MachineOperand`-based conversion module

### Expected size reduction
`ir/codegen.c` was reduced from ~2331 to 1767 lines (Phase 5m deleted `fill_registers_ir` ~256 lines; Phase 6 consolidated dispatch loops −339 lines).

## Verification Checklist

- [x] `ir/mat.c` deleted
- [x] `ir/operand.c` deleted
- [x] `ir/operand.h` deleted
- [x] Build compiles without those files
- [x] `make test -j16` passes
- [x] `tcc_ir_fill_registers_ir()` deleted from `ir/codegen.c` — ✅ done (Phase 5m)
- [x] `ir/codegen.c` reduced from ~2331 to 1767 lines (Phase 5m + Phase 6 dispatch consolidation)
