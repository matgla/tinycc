# Phase 0: Eliminate SValue from Codegen Path

> **Status: ✅ COMPLETE** — committed `e19755e6 new materialization plan`

## Goal

Remove the `SValue`-based materialization and register fill paths. All backend codegen uses `IROperand` exclusively.

## Current State

`ir/mat.c` has **two complete parallel APIs**:

| SValue API (legacy) | IROperand API |
|---|---|
| `tcc_ir_materialize_value(ir, sv, result)` | `tcc_ir_materialize_value_ir(ir, op, result)` |
| `tcc_ir_materialize_const_to_reg(ir, sv, result)` | `tcc_ir_materialize_const_to_reg_ir(ir, op, result)` |
| `tcc_ir_materialize_addr(ir, sv, result, dest_reg)` | `tcc_ir_materialize_addr_ir(ir, op, result, dest_reg)` |
| `tcc_ir_materialize_dest(ir, dest, result)` | `tcc_ir_materialize_dest_ir(ir, op, result)` |
| `tcc_ir_fill_registers(ir, sv)` | `tcc_ir_fill_registers_ir(ir, op)` |

Additionally, there's a **third wrapper layer** (`tcc_ir_mat_value`, `tcc_ir_mat_const`, `tcc_ir_mat_addr`, `tcc_ir_mat_dest`, etc.) that wraps the legacy implementations with newer result types (`TCCMatValue`, `TCCMatDest`, `TCCMatAddr`).

`ir/codegen.c` only uses the IROperand versions (`_ir` suffix) in its main `tcc_ir_codegen_generate()` dispatch loop. The SValue versions may still be called from other paths.

## Files Affected

| File | Changes |
|---|---|
| `ir/mat.c` | Delete all SValue-based functions (~400 lines) |
| `ir/codegen.c` | Remove `tcc_ir_fill_registers()` (SValue version, ~170 lines) |
| `svalue.h` | No changes (SValue struct stays for parser use) |
| `tccgen.c` | No changes (parser keeps using SValue) |
| `tccir.h` | Remove `TCCMaterializedValue`/`Addr`/`Dest` SValue struct declarations |

## Implementation Steps

### Step 0.1: Audit SValue materialization callers

**Action:** Find all call sites of the SValue-based materialization functions.

```bash
grep -rn 'tcc_ir_materialize_value\b' --include='*.c' --include='*.h'
grep -rn 'tcc_ir_materialize_const_to_reg\b' --include='*.c' --include='*.h'
grep -rn 'tcc_ir_materialize_addr\b' --include='*.c' --include='*.h'
grep -rn 'tcc_ir_materialize_dest\b' --include='*.c' --include='*.h'
grep -rn 'tcc_ir_fill_registers\b' --include='*.c' --include='*.h'
grep -rn 'tcc_ir_mat_value\b' --include='*.c' --include='*.h'
grep -rn 'tcc_ir_mat_const\b' --include='*.c' --include='*.h'
grep -rn 'tcc_ir_mat_addr\b' --include='*.c' --include='*.h'
grep -rn 'tcc_ir_mat_dest\b' --include='*.c' --include='*.h'
```

**Expected:** SValue versions are only called from `ir/codegen.c` legacy paths and possibly `arm-thumb-callsite.c`. If there are callers in `arm-thumb-gen.c`, those need conversion first.

**Decision point:** If SValue callers exist outside `ir/codegen.c`, they must be converted to IROperand equivalents before deletion.

### Step 0.2: Identify dead SValue code paths in codegen

**Action:** Check if there's a legacy dispatch loop in `ir/codegen.c` that uses SValue alongside the main IROperand dispatch loop.

Look at `ir/codegen.c` around lines 1800–2300 for a second `switch(cq->op)` block. The file has **4 occurrences** of `case TCCIR_OP_ADD:`, suggesting at least 2 distinct dispatch paths, possibly more (one for need_* classification, one for actual dispatch, potentially a legacy SValue path, and a 64-bit path).

**Decision point:** Determine which dispatch paths are truly dead vs. conditionally active.

### Step 0.3: Delete SValue materialization functions from `ir/mat.c`

**Action:** Remove the following functions:

1. `tcc_ir_materialize_value()` (L69)
2. `tcc_ir_materialize_const_to_reg()` (L186)
3. `tcc_ir_materialize_addr()` (L262)
4. `tcc_ir_materialize_dest()` (L345)
5. `tcc_ir_mat_value()` (L924) — wrapper
6. `tcc_ir_mat_const()` (L937) — wrapper
7. `tcc_ir_mat_addr()` (L950) — wrapper
8. `tcc_ir_mat_dest()` (L963) — wrapper
9. `tcc_ir_mat_spilled()` (L902) — if no remaining callers
10. `tcc_ir_operand_needs_dereference()` (L1071) — if SValue-only

Also remove static helpers only used by SValue path: `mat_slot_sv()`, `mat_offset_sv()`.

### Step 0.4: Delete `tcc_ir_fill_registers()` (SValue version) from `ir/codegen.c`

**Action:** Remove lines ~23–189 (the SValue `tcc_ir_fill_registers` function). Keep `tcc_ir_fill_registers_ir()` (lines ~190–350).

### Step 0.5: Remove SValue struct declarations from `tccir.h`

**Action:** Remove `TCCMaterializedValue`, `TCCMaterializedAddr`, `TCCMaterializedDest` if no IROperand code still uses them. Check if the `_ir` functions still return these types — if so, those structs stay until Phase 4.

**Important:** Do NOT remove `TCCMatValue`/`TCCMatAddr`/`TCCMatDest` (the newer wrapper types) if they're used by IROperand functions.

### Step 0.6: Compile and test

```bash
make clean && make cross -j16
make test -j16
```

**Expected:** All tests pass. This is a pure dead-code removal with no behavior change.

## Risk Assessment

- **Risk: Low.** This is dead code removal. The SValue functions are a legacy path.
- **Risk: Medium** if the SValue functions are still reachable through conditional compilation or runtime paths. The audit in Step 0.1 will reveal this.
- **Mitigation:** `grep` thoroughly, compile with `-Werror -Wunused-function` to catch orphaned static helpers.

## Verification Checklist

- [x] All SValue materialization callers identified and removed/converted
- [x] No `tcc_ir_materialize_value\b` (non-`_ir`) references remain
- [x] No `tcc_ir_fill_registers\b` (non-`_ir`) references remain
- [x] `make cross` compiles without warnings
- [x] `make test -j16` passes
- [x] `ir/mat.c` SValue functions deleted (later: whole file deleted in Phase 4)
