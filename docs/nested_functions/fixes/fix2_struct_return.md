# Fix 2: `nested_struct_return.c` — Struct Return from Nested Functions

**Test**: `tests/ir_tests/nested_struct_return.c`
**Error**: Type mismatch / incorrect codegen for struct return via sret
**Root Cause**: sret (struct return) ABI interaction with nested function static chain
**Complexity**: Medium
**Depends on**: Fix 1 (captured_types propagation)

## Problem

The nested function `Point offset(Point p)` returns a `Point` (8 bytes). On ARM, `gfunc_sret()` (`arm-thumb-gen.c:2165`) returns 0 for structs > 4 bytes, meaning the sret convention is used: a hidden first parameter (pointer to caller-allocated return buffer) is passed in R0.

The interaction between `SET_CHAIN` (R10 = parent FP) and the sret hidden pointer needs verification. Possible failure modes:

1. Parameter numbering is off — the sret pointer is param #0, but call_id encoding may not account for it correctly alongside SET_CHAIN
2. The nested function's `gen_function()` doesn't correctly set up the implicit sret parameter when `has_static_chain` is also active
3. Type propagation issues (resolved by Fix 1's `captured_types` change—`dx` and `dy` are `int` which was already correct, but other captured types may be wrong)

## Diagnostic Steps

### 1. Compile with IR dump

```bash
cd tests/ir_tests
python run.py -c nested_struct_return.c --dump-ir
```

Examine the IR around the `offset(p)` call. Check:
- `SET_CHAIN` emission relative to `FUNCPARAMVAL` for sret pointer
- `FUNCPARAMVAL` numbering: sret = param #0, `p` = param #1
- The nested `offset` function's prologue: sret hidden param + static chain

### 2. Disassemble

```bash
arm-none-eabi-objdump -d tests/ir_tests/build/nested_struct_return.elf | grep -A 30 'offset\.'
```

Check register usage: R0 = sret pointer (hidden), R1-R2 = Point p (8 bytes), R10 = chain (parent FP).

## Changes

### 1. Verify SET_CHAIN / sret ordering (`tccgen.c:~7520-7600`)

The `SET_CHAIN` IR op is emitted at `tccgen.c:7531` **before** any `FUNCPARAMVAL` instructions. The sret hidden pointer is emitted as `FUNCPARAMVAL` at `tccgen.c:7575-7584`. This ordering should be correct:

- `SET_CHAIN` → sets R10 (not a register parameter, no conflict)
- `FUNCPARAMVAL` param #0 → sret pointer in R0
- `FUNCPARAMVAL` param #1 → Point p in R1-R2

Verify this is the actual ordering in the IR dump. If not, fix the emission sequence.

### 2. Check nested function prologue (`ir/core.c:~599`)

When the nested `offset` function is compiled:
- `gfunc_sret()` detects struct return → sret convention
- `gen_function()` creates the implicit sret parameter (func_vc)
- The static chain (R10) is set up as a separate vreg, NOT as a parameter

Ensure the parameter list setup in `ir/core.c` correctly handles sret + static chain together. The sret pointer should be parameter #0 (in R0), and `Point p` should be parameter #1 (in R1-R2). R10 is independent.

### 3. Fix any parameter count mismatch

If the sret hidden parameter is counted differently when `has_static_chain` is set, fix the count. The chain is NOT a parameter in the AAPCS sense—it uses R10, not R0-R3.

### 4. Apply Fix 1 first

The `captured_types` fix ensures `dx` and `dy` have correct types. While they happen to be `int` (matching the hardcoded `VT_INT`), having real types prevents fragile assumptions.

### 5. Remove xfail (`tests/ir_tests/test_qemu.py:~288`)

Remove `("nested_struct_return.c", 0)` from `NESTED_XFAIL_TEST_FILES`.

## Verification

```bash
cd tests/ir_tests && python run.py -c nested_struct_return.c --dump-ir
make test -j16  # no regressions
```
