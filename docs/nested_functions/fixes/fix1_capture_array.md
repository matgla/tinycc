# Fix 1: `nested_capture_array.c` — Array Capture Type Propagation

**Test**: `tests/ir_tests/nested_capture_array.c`
**Error**: "pointer expected" — `arr[i]` fails because captured `arr` has type `VT_INT` instead of `int[5]`
**Root Cause**: Captured variable type hardcoded to `VT_INT` at `tccgen.c:7376`
**Complexity**: Low

## Problem

When a nested function references a parent variable, the captured-var resolver at `tccgen.c:7376` creates a fake symbol with:

```c
s->type.t = VT_INT; /* Default to int - type will be cast later if needed */
```

For arrays, this means `arr` is treated as a plain `int`, so applying `[]` to it triggers "pointer expected". The real type (`int[5]`) is never propagated.

## Changes

### 1. Add `captured_types[]` to `NestedFunc` (`tcc.h:~722`)

Add a `CType` array to store the full type of each captured variable:

```c
typedef struct NestedFunc
{
  // ... existing fields ...
  int captured_offsets[MAX_CAPTURED_VARS];
  int captured_tokens[MAX_CAPTURED_VARS];
  int captured_vregs[MAX_CAPTURED_VARS];
  CType captured_types[MAX_CAPTURED_VARS];  // <-- NEW: full type of captured vars
  int nb_captured;
  // ...
} NestedFunc;
```

### 2. Record parent symbol's `CType` in `prescan_captured_vars()` (`tccgen.c:~11198`)

When a captured variable is recorded, also store its type:

```c
if (!already_captured && nf->nb_captured < MAX_CAPTURED_VARS)
{
  nf->captured_vregs[nf->nb_captured] = s->vreg;
  nf->captured_offsets[nf->nb_captured] = s->c;
  nf->captured_tokens[nf->nb_captured] = t;
  nf->captured_types[nf->nb_captured] = s->type;  // <-- NEW
  nf->nb_captured++;
}
```

### 3. Use real type in captured-var resolver (`tccgen.c:~7376`)

Replace the hardcoded `VT_INT` with the actual captured type:

```c
// BEFORE:
s->type.t = VT_INT;

// AFTER:
s->type = nf->captured_types[i];
```

### 4. Remove xfail (`tests/ir_tests/test_qemu.py:~289`)

Remove `("nested_capture_array.c", 0)` from `NESTED_XFAIL_TEST_FILES`.

## Why This Works

- Arrays accessed via the static chain: the chain-relative offset (R10 + parent FP offset) points to the start of the array in the parent's stack frame
- With the correct `VT_ARRAY` type, the `[]` operator triggers normal array-to-pointer decay (`gaddrof()`) + index arithmetic
- ARM codegen at `arm-thumb-gen.c:2282-2294` already handles arbitrary offsets from R10 — no backend changes needed

## Verification

```bash
cd tests/ir_tests && python run.py -c nested_capture_array.c --dump-ir
make test -j16  # no regressions
```
