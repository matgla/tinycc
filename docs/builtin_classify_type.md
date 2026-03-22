# `__builtin_classify_type` Implementation Plan

## Overview

GCC's `__builtin_classify_type(expr)` is a compile-time builtin that returns an integer constant classifying the type of its argument expression. It is used in `<tgmath.h>` and GCC torture tests (e.g., `20040709-1.c`, `20040709-2.c`) to detect floating-point types at compile time.

The builtin evaluates at **compile time only** — the argument expression is parsed for its type but **never emitted as code** (similar to `sizeof`).

## GCC Type Classification Values

| Value | GCC Enum Constant         | Type Category                        |
|-------|---------------------------|--------------------------------------|
| 0     | `no_type_class`           | void                                 |
| 1     | `integer_type_class`      | integer types (char, short, int, long, long long, _Bool, enum) |
| 2     | `char_type_class`         | **not used in C** (only C++ plain `char`) |
| 3     | `enumeral_type_class`     | **not used in C** (C enums → integer) |
| 4     | `boolean_type_class`      | **not used in C** (C _Bool → integer) |
| 5     | `pointer_type_class`      | pointer types                        |
| 6     | `reference_type_class`    | **C++ only** — references            |
| 7     | `offset_type_class`       | **C++ only** — pointer-to-member     |
| 8     | `real_type_class`         | float, double, long double           |
| 9     | `complex_type_class`      | _Complex float/double/long double    |
| 10    | `function_type_class`     | function types (bare function, not pointer-to-function) |
| 11    | `method_type_class`       | **C++ only** — method types          |
| 12    | `record_type_class`       | struct                               |
| 13    | `union_type_class`        | union                                |
| 14    | `array_type_class`        | array types                          |
| 15    | `string_type_class`       | **not used in C**                    |
| 16    | `opaque_type_class`       | **not used in C**                    |
| 17    | `bitint_type_class`       | _BitInt (GCC 14+)                    |
| 18    | `vector_type_class`       | GCC vector types (`__attribute__((vector_size(...)))`) |

### Key Observations for C (what TCC needs)

In practice for C code, only these values appear:

- **0** — `void`
- **1** — all integer types (`char`, `short`, `int`, `long`, `long long`, `_Bool`, enums)
- **5** — pointers (including pointer-to-function, arrays decay to pointers in expressions)
- **8** — `float`, `double`, `long double`
- **9** — `_Complex` types (if supported)
- **12** — `struct`
- **13** — `union`
- **14** — array types (when passed as a type, not decayed)

Note: In GCC's C mode, `enum` maps to **1** (integer), not 3. `_Bool` also maps to **1**, not 4.

## TCC Type System Mapping

The mapping from TCC's `VT_*` type flags to GCC classification values:

| TCC Type (`VT_BTYPE`)      | TCC Flags                              | GCC Classification |
|-----------------------------|----------------------------------------|--------------------|
| `VT_VOID` (0)              | —                                      | 0 (void)           |
| `VT_BYTE` (1)              | ± `VT_UNSIGNED`                        | 1 (integer)        |
| `VT_SHORT` (2)             | ± `VT_UNSIGNED`                        | 1 (integer)        |
| `VT_INT` (3)               | ± `VT_UNSIGNED`, ± `VT_ENUM`          | 1 (integer)        |
| `VT_LLONG` (4)             | ± `VT_UNSIGNED`                        | 1 (integer)        |
| `VT_PTR` (5)               | without `VT_ARRAY`                     | 5 (pointer)        |
| `VT_PTR` (5)               | with `VT_ARRAY`                        | 14 (array)         |
| `VT_FUNC` (6)              | —                                      | 10 (function)      |
| `VT_STRUCT` (7)            | without `VT_UNION` high bits           | 12 (record/struct) |
| `VT_STRUCT` (7)            | with `VT_UNION` high bits (`IS_UNION`) | 13 (union)         |
| `VT_FLOAT` (8)             | without `VT_COMPLEX`                   | 8 (real)           |
| `VT_DOUBLE` (9)            | without `VT_COMPLEX`                   | 8 (real)           |
| `VT_LDOUBLE` (10)          | without `VT_COMPLEX`                   | 8 (real)           |
| `VT_FLOAT` (8)             | with `VT_COMPLEX`                      | 9 (complex)        |
| `VT_DOUBLE` (9)            | with `VT_COMPLEX`                      | 9 (complex)        |
| `VT_LDOUBLE` (10)          | with `VT_COMPLEX`                      | 9 (complex)        |
| `VT_BOOL` (11)             | —                                      | 1 (integer)        |
| any with `VT_VECTOR`       | —                                      | 18 (vector) *optional* |

## Implementation Steps

### Step 1: Add Token Definition

In `tcctok.h`, add near the other `__builtin_*` tokens (~line 190):

```c
DEF(TOK_builtin_classify_type, "__builtin_classify_type")
```

### Step 2: Add Classification Helper Function

In `tccgen.c`, add a static helper that maps a `CType` to the GCC integer:

```c
/* GCC __builtin_classify_type return values (C mode) */
#define GCC_TYPE_CLASS_VOID      0
#define GCC_TYPE_CLASS_INTEGER   1
#define GCC_TYPE_CLASS_POINTER   5
#define GCC_TYPE_CLASS_REAL      8
#define GCC_TYPE_CLASS_COMPLEX   9
#define GCC_TYPE_CLASS_FUNCTION  10
#define GCC_TYPE_CLASS_STRUCT    12
#define GCC_TYPE_CLASS_UNION     13
#define GCC_TYPE_CLASS_ARRAY     14
#define GCC_TYPE_CLASS_VECTOR    18

static int gcc_classify_type(CType *type)
{
    int bt = type->t & VT_BTYPE;
    int t = type->t;

    switch (bt) {
    case VT_VOID:
        return GCC_TYPE_CLASS_VOID;

    case VT_BYTE:
    case VT_SHORT:
    case VT_INT:
    case VT_LLONG:
    case VT_BOOL:
        return GCC_TYPE_CLASS_INTEGER;

    case VT_PTR:
        if (t & VT_ARRAY)
            return GCC_TYPE_CLASS_ARRAY;
        return GCC_TYPE_CLASS_POINTER;

    case VT_FUNC:
        return GCC_TYPE_CLASS_FUNCTION;

    case VT_STRUCT:
        if (IS_UNION(t))
            return GCC_TYPE_CLASS_UNION;
        return GCC_TYPE_CLASS_STRUCT;

    case VT_FLOAT:
    case VT_DOUBLE:
    case VT_LDOUBLE:
        if (t & VT_COMPLEX)
            return GCC_TYPE_CLASS_COMPLEX;
        return GCC_TYPE_CLASS_REAL;

    default:
        return GCC_TYPE_CLASS_INTEGER; /* fallback */
    }
}
```

### Step 3: Add Parser Case in `unary()`

In the `unary()` function in `tccgen.c`, add a case alongside the other `TOK_builtin_*` cases (near `TOK_builtin_constant_p`):

```c
case TOK_builtin_classify_type:
    parse_builtin_params(1, "e");   /* nc=1: nocode, "e": one expression */
    n = gcc_classify_type(&vtop->type);
    vtop--;
    vpushi(n);
    break;
```

Key details:
- **`nc=1`** — increments `nocode_wanted` so the argument expression is parsed but no code is generated (just like `sizeof`).
- **`"e"`** — parse one expression argument.
- After parsing, inspect `vtop->type` to get the type, pop it, and push the integer constant result.

### Step 4: Add Test

Create `tests/ir_tests/NN_builtin_classify_type.c`:

```c
#include <stdio.h>

struct S { int x; };
union U { int x; float f; };

int main(void)
{
    int i = 0;
    float f = 0.0f;
    double d = 0.0;
    int *p = &i;
    struct S s;
    union U u;
    int arr[4];
    void (*fp)(void);

    printf("%d\n", __builtin_classify_type(i));     /* 1 - integer */
    printf("%d\n", __builtin_classify_type(f));     /* 8 - real */
    printf("%d\n", __builtin_classify_type(d));     /* 8 - real */
    printf("%d\n", __builtin_classify_type(p));     /* 5 - pointer */
    printf("%d\n", __builtin_classify_type(s));     /* 12 - struct */
    printf("%d\n", __builtin_classify_type(u));     /* 13 - union */
    printf("%d\n", __builtin_classify_type(0));     /* 1 - integer */
    printf("%d\n", __builtin_classify_type(0.0));   /* 8 - real */
    printf("%d\n", __builtin_classify_type((char)0)); /* 1 - integer */
    return 0;
}
```

Corresponding `.expect` file:
```
1
8
8
5
12
13
1
8
1
```

### Step 5: Verify GCC Torture Tests

After implementation, verify the two GCC torture tests that use this builtin pass:
```bash
cd tests/ir_tests
python run.py -c ../gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/20040709-1.c --cflags="-O1"
python run.py -c ../gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/20040709-2.c --cflags="-O1"
```

## Edge Cases & Notes

1. **Array vs pointer**: `__builtin_classify_type(arr)` where `arr` is `int[4]` — GCC returns 5 (pointer) because the expression `arr` decays to a pointer. However `__builtin_classify_type((int[4]){})` on a compound literal that hasn't decayed should return 14 (array). In practice, since TCC parses the argument as an expression, array-to-pointer decay will already have occurred, so this should naturally return 5 for array names — matching GCC behavior.

2. **Function vs function pointer**: `__builtin_classify_type(main)` — the function name decays to a function pointer, so GCC returns 5 (pointer). This should work naturally.

3. **String literals**: `__builtin_classify_type("hello")` — the string literal is `char[6]` which decays to `char*`, so returns 5 (pointer).

4. **No side effects**: The argument must not generate any code. The `nocode_wanted` flag via `parse_builtin_params(1, ...)` handles this.

5. **`_Complex` types**: If/when TCC supports `_Complex`, the `VT_COMPLEX` flag check ensures correct classification (value 9).

6. **`VT_VECTOR` types**: Optionally return 18 for GCC vector types if `VT_VECTOR` is set. This is a GCC 14+ addition and low priority.

## Files to Modify

| File         | Change                                              |
|--------------|-----------------------------------------------------|
| `tcctok.h`   | Add `TOK_builtin_classify_type` token definition    |
| `tccgen.c`   | Add `gcc_classify_type()` helper + `case` in `unary()` |

## Estimated Effort

Small — ~30 lines of code across 2 files, plus test file. The implementation is entirely compile-time (no IR or codegen changes needed).
