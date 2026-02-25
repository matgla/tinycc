# Fix: Large Struct Pass-by-Value Broken for Implicitly Declared Functions

**Test case**: `gcc.c-torture/execute/20000412-3.c`
**Symptom**: Exit code 1 (abort) with `-O0`.

## Test Case

```c
typedef struct {
  char y;
  char x[32];
} X;  /* sizeof(X) == 33 bytes */

int z(void)
{
  X xxx;
  xxx.x[0] = xxx.x[31] = '0';
  xxx.y = 0xf;
  return f(xxx, xxx);  /* f() not yet declared — implicit declaration */
}

int main(void)
{
  int val = z();
  if (val != 0x60)
    abort();
  exit(0);
}

int f(X x, X y)
{
  if (x.y != y.y)
    return 'F';
  return x.x[0] + y.x[0];  /* expected: '0' + '0' = 0x60 = 96 */
}
```

Expected: `f` returns `0x60` (96). Actual: exit code 1 (abort).

## Root Cause

The struct `X` is 33 bytes. Per ARM AAPCS, composite types larger than 16 bytes
must be passed via **invisible reference** — the caller allocates a copy on the
stack and passes a pointer to that copy.

### Callee side (correct)

When `f(X x, X y)` is compiled, the compiler knows it has 33-byte struct
parameters. The IR treats `P0`/`P1` as 4-byte pointers and dereferences them:

```
0002: T0 <-- StackLoc[-4] [LOAD]    ; reload pointer
0004: T2 <-- T0***DEREF*** [LOAD]   ; dereference: x.y = *(pointer)
```

The generated ARM correctly uses `ldrb r2, [r0, #0]` (indirect load through
pointer).

### Caller side (broken)

When `z()` calls `f(xxx, xxx)`, the function `f` has **no visible prototype**
(it's declared after `z`). The compiler sees it as `FUNC_OLD` (K&R-style /
implicit declaration).

The IR emits:

```
0009: PARAM0[call_0] StackLoc[-33]
0010: PARAM1[call_0] StackLoc[-33]
0011: CALL GlobalSym(935) --> T6
```

These are raw struct values at `StackLoc[-33]`, not pointers to copies.

The generated ARM loads the **first 4 bytes of the struct value** instead of
passing the struct's address:

```arm
sub.w   ip, r7, #33      ; ip = &xxx (address of struct on stack)
ldr.w   r0, [ip]         ; BUG: r0 = first 4 bytes of struct DATA
sub.w   ip, r7, #33
ldr.w   r1, [ip]         ; BUG: r1 = first 4 bytes of struct DATA
bl      f
```

The callee then dereferences these garbage "pointers" (actually `0x0f303030`
or similar), causing a wrong result or crash.

### The mismatch

| | Caller (`z`) | Callee (`f`) |
|---|---|---|
| **Sees `f` as** | `int f()` (implicit, no param info) | `int f(X x, X y)` (33-byte struct params) |
| **Passes in r0/r1** | First 4 bytes of struct value | Expects pointers to struct copies |

## Bug Location

**File**: `tccgen.c`, function `gfunc_param_typed` (line ~6469)

The AAPCS invisible-reference conversion for large structs (lines 6505–6552)
is inside the `else` branch that only executes when a proper prototype exists
(`arg != NULL`):

```c
static void gfunc_param_typed(Sym *func, Sym *arg)
{
  func_type = func->f.func_type;
  if (func_type == FUNC_OLD || (func_type == FUNC_ELLIPSIS && arg == NULL))
  {
    /* default casting : only need to convert float to double */
    if ((vtop->type.t & VT_BTYPE) == VT_FLOAT)
      gen_cast_s(VT_DOUBLE);
    // ... other default casts ...
    // *** NO large-struct handling here! ***
  }
  else if (arg == NULL)
  {
    tcc_error("too many arguments to function");
  }
  else
  {
    // ... prototype-aware path ...
    if ((type.t & VT_BTYPE) == VT_STRUCT)
    {
      int align, size = type_size(&type, &align);
      if (size > 16)
      {
        /* AAPCS invisible reference: allocate temp copy, pass pointer */
        // ... mk_pointer() + gaddrof() ...
      }
    }
    gen_assign_cast(&type);
  }
}
```

The `FUNC_OLD` path (lines 6475–6493) handles only `float→double` promotion,
bitfield casts, and `VT_MUSTCAST`. It has **no handling for large structs**.

## Proposed Fix

Add large-struct invisible-reference handling to the `FUNC_OLD` / no-prototype
path, since the ABI convention must be followed regardless of whether a
prototype is visible.

### Fix: Add AAPCS struct handling to the FUNC_OLD path

In `gfunc_param_typed`, at the top of the `FUNC_OLD` branch (line ~6477),
before existing default casting:

```c
if (func_type == FUNC_OLD || (func_type == FUNC_ELLIPSIS && arg == NULL))
{
  /* ARM AAPCS: large structs must use invisible reference even without
   * a prototype, since the ABI is a property of the callee's compiled
   * code, not the caller's view of the declaration. */
  if ((vtop->type.t & VT_BTYPE) == VT_STRUCT)
  {
    int align, size = type_size(&vtop->type, &align);
    if (size > 16)
    {
      if (nocode_wanted)
        return;
      if (!(vtop->r & VT_LVAL))
        tcc_error("cannot pass large struct by value");

      int temp_vr;
      int tmp_loc = get_temp_local_var(size, align, &temp_vr);

      SValue dst;
      memset(&dst, 0, sizeof(dst));
      dst.type = vtop->type;
      dst.r = VT_LOCAL | VT_LVAL;
      dst.vr = temp_vr;
      dst.c.i = tmp_loc;
      vpushv(&dst);
      vswap();
      vstore();

      mk_pointer(&vtop->type);
      gaddrof();
      return;
    }
  }

  /* existing default casting: float to double, etc. */
  if ((vtop->type.t & VT_BTYPE) == VT_FLOAT)
  {
    gen_cast_s(VT_DOUBLE);
  }
  // ...
}
```

This duplicates the logic from the prototype-aware path (lines 6505–6552) but
uses `vtop->type` (the actual argument type) instead of `arg->type` (the
parameter type from the prototype, which doesn't exist here).

### Alternative: Extract shared helper

To avoid duplication, extract a helper function:

```c
/* Convert a large struct argument to an invisible-reference pointer (AAPCS).
 * Returns 1 if conversion was applied, 0 otherwise. */
static int maybe_convert_large_struct_to_ref(CType *type)
{
  if ((type->t & VT_BTYPE) != VT_STRUCT)
    return 0;
  int align, size = type_size(type, &align);
  if (size <= 16)
    return 0;
  if (nocode_wanted)
    return 1;
  if (!(vtop->r & VT_LVAL))
    tcc_error("cannot pass large struct by value");

  int temp_vr;
  int tmp_loc = get_temp_local_var(size, align, &temp_vr);

  SValue dst;
  memset(&dst, 0, sizeof(dst));
  dst.type = *type;
  dst.r = VT_LOCAL | VT_LVAL;
  dst.vr = temp_vr;
  dst.c.i = tmp_loc;
  vpushv(&dst);
  vswap();
  vstore();

  mk_pointer(&vtop->type);
  gaddrof();
  return 1;
}
```

Then call it from both paths in `gfunc_param_typed`:

```c
if (func_type == FUNC_OLD || (func_type == FUNC_ELLIPSIS && arg == NULL))
{
  if (maybe_convert_large_struct_to_ref(&vtop->type))
    return;
  /* existing default casts ... */
}
else
{
  type = arg->type;
  type.t &= ~VT_CONSTANT;
  if (maybe_convert_large_struct_to_ref(&type))
    return;
  gen_assign_cast(&type);
}
```

## Disassembly Comparison

### Current (broken):

```arm
; z() calling f():
sub.w   ip, r7, #33      ; ip = &xxx
ldr.w   r0, [ip]         ; r0 = WRONG: loads struct bytes 0-3
sub.w   ip, r7, #33
ldr.w   r1, [ip]         ; r1 = WRONG: loads struct bytes 0-3
bl      f
```

### Expected (after fix):

```arm
; z() calling f():
; allocate temp copy 1 on stack, memcpy xxx into it
; allocate temp copy 2 on stack, memcpy xxx into it
; r0 = pointer to temp copy 1
; r1 = pointer to temp copy 2
bl      f
```

## Testing

1. Verify the test passes:
   ```bash
   cd tests/ir_tests
   python run.py -c ../gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/20000412-3.c --cflags="-O0"
   ```

2. Run the full test suite to check for regressions:
   ```bash
   make test -j16
   make test-all
   ```

3. Also test with a prototype-visible variant to confirm no regression:
   ```c
   int f(X x, X y);  /* forward declaration */
   int z(void) { X xxx; ... return f(xxx, xxx); }
   ```

## Risk Assessment

**Low risk.** The fix adds handling to a code path that previously had none for
this case. It only affects `FUNC_OLD` (implicit/K&R) calls with struct arguments
larger than 16 bytes — a narrow and well-defined scenario. The same conversion
logic already works correctly for prototype-visible calls.

One caveat: if the callee is compiled by a different compiler that does NOT use
invisible references for large structs on `FUNC_OLD` calls, there would be an
ABI mismatch. However, GCC and Clang both follow the AAPCS regardless of
prototype visibility, so this fix aligns TCC with standard behavior.
