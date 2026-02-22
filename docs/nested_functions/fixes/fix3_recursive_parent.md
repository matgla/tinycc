# Fix 3: `nested_recursive_parent.c` — Scope Resolution for Parameters

**Test**: `tests/ir_tests/nested_recursive_parent.c`
**Error**: "undeclared" — captured variable `n` (parameter) or `result` (local) not found
**Root Cause**: `prescan_captured_vars()` filter condition may reject parameter symbols
**Complexity**: Low

## Problem

`factorial_with_nested(int n)` is a file-scope function containing nested function `accumulate()` which captures both:
- `result` — local variable
- `n` — function parameter

The phase2 doc states this fails with "'n' undeclared" or similar. The prescan at `tccgen.c:11178` uses:

```c
Sym *s = sym_find2(parent_local_stack, t);
if (s && (s->r & VT_VALMASK) == VT_LOCAL)
```

Function parameters are pushed onto `local_stack` during `gen_function()` and should have `VT_LOCAL` in their `r` field. However, they may also carry `VT_PARAM` or other flags that cause the `VT_VALMASK` check to reject them.

The **alternative theory**: since `factorial_with_nested` is a file-scope function (not itself nested), `decl(VT_LOCAL)` handles the nested definition inside its body. The `local_stack` at prescan time should include both `n` (parameter, pushed by `gen_function`) and `result` (local, pushed by `decl_initializer_alloc`). If parameters are pushed AFTER `block(0)` starts but the nested function definition comes before `result` is declared, then the ordering matters.

## Diagnostic Steps

### 1. Add debug output to prescan

Temporarily add to `prescan_captured_vars()`:
```c
fprintf(stderr, "PRESCAN: token=%s sym=%p r=0x%x valmask=0x%x\n",
        get_tok_str(t, NULL), s, s ? s->r : 0, s ? (s->r & VT_VALMASK) : 0);
```

### 2. Compile and check

```bash
./armv8m-tcc -c tests/ir_tests/nested_recursive_parent.c 2>&1 | head -20
```

Check which tokens are scanned, whether `result` and `n` are found on `parent_local_stack`, and what their `s->r` values are.

## Changes

### 1. Fix prescan filter condition (`tccgen.c:~11180`)

If the diagnostic shows parameters have flags beyond `VT_LOCAL`, broaden the check:

```c
// BEFORE:
if (s && (s->r & VT_VALMASK) == VT_LOCAL)

// AFTER (option A — also accept parameters explicitly):
if (s && ((s->r & VT_VALMASK) == VT_LOCAL || (s->r & VT_PARAM)))

// AFTER (option B — accept any stack-resident symbol):
if (s && ((s->r & VT_VALMASK) == VT_LOCAL))
// (if VT_PARAM symbols already have VT_LOCAL in VT_VALMASK, this is already correct
//  and the issue is elsewhere)
```

The exact fix depends on the diagnostic output. If parameters already have `(s->r & VT_VALMASK) == VT_LOCAL`, the prescan filter is fine and the issue is in the captured-var resolver at `tccgen.c:7370`—possibly the resolver can't match because the token ID differs for parameters vs locals.

### 2. Verify parameter offset stability

Parameters' FP offsets are deterministic (assigned during `gen_function()` before `block(0)`). Since `prescan_captured_vars` runs during `block(0) → decl(VT_LOCAL)`, the parameter's `s->c` should be correct. Verify that `captured_offsets[]` gets the right value for `n`.

### 3. Verify recursion correctness (no code changes expected)

Each recursive call to `factorial_with_nested` creates a new stack frame. At each call to `accumulate()`:
- `SET_CHAIN` copies the current FP to R10
- `accumulate()` accesses `result` and `n` via R10 + offset
- This correctly accesses the current invocation's variables

No codegen changes needed for recursion support.

### 4. Apply Fix 1 (`captured_types`)

With the `captured_types` change from Fix 1, `result` and `n` will have correct `int` type (already `VT_INT` by coincidence, but proper propagation is better).

### 5. Remove xfail (`tests/ir_tests/test_qemu.py:~287`)

Remove `("nested_recursive_parent.c", 0)` from `NESTED_XFAIL_TEST_FILES`.

## Verification

```bash
cd tests/ir_tests && python run.py -c nested_recursive_parent.c --dump-ir
make test -j16  # no regressions
```
