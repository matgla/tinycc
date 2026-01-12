# Plan: Simplify load_to_dest() by Removing VT_CONST/VT_CMP/VT_JMP Handling

## Background

`load_to_dest()` in [arm-thumb-gen.c:2756](arm-thumb-gen.c#L2756) is a ~200 line "do-everything" function that handles:
- VT_LVAL (memory loads via addresses)
- VT_CONST (constant materialization)
- VT_LOCAL (address computation)
- VT_CMP (comparison result materialization)
- VT_JMP/VT_JMPI (jump condition materialization)
- Register-to-register moves

The new IR-level machine APIs already provide the same constant/cmp/jmp materialization:
- `tcc_machine_load_constant()` - [arm-thumb-gen.c:2364](arm-thumb-gen.c#L2364)
- `tcc_machine_load_cmp_result()` - [arm-thumb-gen.c:2403](arm-thumb-gen.c#L2403)
- `tcc_machine_load_jmp_result()` - [arm-thumb-gen.c:2419](arm-thumb-gen.c#L2419)

These are called by `tcc_ir_materialize_const_to_reg()` in tccir.c.

## Goal

Remove VT_CONST/VT_CMP/VT_JMP handling from `load_to_dest()` and ensure all callers use the appropriate APIs directly.

## Current Callers Analysis

| Location | Caller | What it passes | Action needed |
|----------|--------|----------------|---------------|
| Line 2972 | `load_to_reg()` wrapper for `load()` | Any SValue | Keep fallback (legacy) |
| Line 3560 | `thumb_emit_logical64_op()` | VT_CONST (folded result) | Replace with `tcc_machine_load_constant()` |
| Line 5058 | `tcc_gen_machine_return_value_op()` | VT_CONST | Replace with `tcc_machine_load_constant()` |
| Line 5077 | `tcc_gen_machine_return_value_op()` | Fallback case | Keep as fallback |
| Line 5086 | `tcc_gen_machine_load_op()` | VT_LVAL (memory load) | Keep - this is VT_LVAL, not const |
| Line 5604 | `tcc_gen_machine_move_op()` | VT_CONST to stack | Replace with `tcc_machine_load_constant()` |
| Line 5613 | `tcc_gen_machine_move_op()` | VT_LOCAL (address) | Keep - this is VT_LOCAL |

## Implementation Steps

### Step 1: Extend tcc_machine_load_constant() to handle symbols

The current `load_vt_const()` handles `VT_SYM` for symbol-relative constants. Extend `tcc_machine_load_constant()` to accept an optional `Sym*` parameter:

```c
ST_FUNC void tcc_machine_load_constant(int dest_reg, int dest_reg_high, int64_t value, int is_64bit, Sym *sym)
```

When `sym` is non-NULL, use `load_full_const()` which handles relocations. Update all existing callers to pass `NULL` for the new parameter.

### Step 2: Update `thumb_emit_logical64_op()` (line 3560)

Replace:
```c
load_to_dest(&op->dest, &folded);
```
With direct call to `tcc_machine_load_constant()`.

### Step 3: Update `tcc_gen_machine_return_value_op()` (line 5058)

Replace:
```c
return load_to_dest(&dest, &q->src1);
```
With direct call to `tcc_machine_load_constant()`, extracting the constant value from `q->src1.c.i`.

### Step 4: Update `tcc_gen_machine_move_op()` VT_CONST case (around line 5604)

Replace `load_to_dest()` call with `tcc_machine_load_constant()` for the constant materialization before storing.

### Step 5: Simplify load_to_dest() VT_CONST/VT_CMP/VT_JMP handlers

Route these cases through the machine APIs:
```c
else if (v == VT_CONST)
{
  Sym *sym = (sv->r & VT_SYM) ? validate_sym_for_reloc(sv->sym) : NULL;
  return tcc_machine_load_constant(dest->pr0, dest->pr1, sv->c.i, tcc_is_64bit_operand(sv), sym);
}
else if (v == VT_CMP)
  return tcc_machine_load_cmp_result(dest->pr0, sv->c.i);
else if (v == VT_JMP || v == VT_JMPI)
  return tcc_machine_load_jmp_result(dest->pr0, sv->c.i, (fr & VT_VALMASK) == VT_JMPI);
```

This consolidates the implementation while preserving backward compatibility.

### Step 6: Delete redundant helper functions

Once `load_to_dest()` uses machine APIs directly:
- `load_vt_const()` can be deleted (logic moved to machine API)
- `load_vt_cmp()` can be deleted (duplicates `tcc_machine_load_cmp_result()`)
- `load_vt_jmp_jmpi()` can be deleted (duplicates `tcc_machine_load_jmp_result()`)

## Files to Modify

1. [arm-thumb-gen.c](arm-thumb-gen.c) - All changes are in this file

## Verification

1. Run the test suite: `make test` or equivalent
2. Specifically test:
   - 64-bit constant folding (tests `thumb_emit_logical64_op` path)
   - Function return values with constants
   - Move operations with constants
   - Comparison results used as values (VT_CMP)
   - Short-circuit boolean expressions (VT_JMP)
   - Symbol-relative constants (global variables, function pointers)

## Risk Assessment

- **Low risk**: The machine APIs already exist and are tested via IR materialization
- **Medium risk**: VT_SYM handling needs careful attention - symbol-relative constants use relocations
- **Mitigation**: Keep the legacy `load()` → `load_to_reg()` path working as fallback during transition

## Summary of Changes

After this refactoring:
- `load_to_dest()` will be simplified to ~150 lines, focusing on:
  - VT_LVAL (memory loads)
  - VT_LOCAL (address computation)
  - Register-to-register moves
- VT_CONST/VT_CMP/VT_JMP cases will route through machine APIs
- Three helper functions will be deleted (reducing code duplication)
- All callers that explicitly pass these value types will call machine APIs directly
