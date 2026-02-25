# Complex Number Support Implementation Plan

This document outlines the plan for adding C99 complex number support (`_Complex`, `__complex__`, `complex.h`) to TinyCC for ARMv8-M.

## Overview

Complex numbers in C99 are defined as:
- `float _Complex` - 8 bytes (2 x float)
- `double _Complex` - 16 bytes (2 x double)  
- `long double _Complex` - 16 bytes (2 x double, same as double _Complex on ARM)

### Current Status (Updated: 2026-02-26)

**Implementation is ~60% complete.** Phases 1-2 are done, Phase 3 is partially complete.

| Phase | Status | Description |
|-------|--------|-------------|
| 1: Type System | ✅ **COMPLETE** | Type parsing, sizeof, conversions work |
| 2: IR Support | ✅ **COMPLETE** | Complex types flow through IR correctly |
| 3: Code Gen | 🚧 **PARTIAL** | Add/sub work, **mul/div missing** |
| 4: Accessors | 🚧 **PARTIAL** | `__real__`/`__imag__` parse, L-values pending |
| 5: Constants | ❌ **NOT STARTED** | `1.0fi` imaginary suffix not implemented |
| 6: Library | ✅ **COMPLETE** | `complex.h` header ready |
| 7: ABI/Calling | 🚧 **PARTIAL** | Basic calls work, edge cases pending |

**What Works:**
```c
_Complex float cf;                    // ✅ Declaration
sizeof(_Complex float);               // ✅ Returns 8
_Complex float c = a + b;             // ✅ Addition
_Complex float d = a - b;             // ✅ Subtraction
```

**What's Missing:**
```c
_Complex float c = a * b;             // ❌ Multiplication not implemented
_Complex float d = a / b;             // ❌ Division not implemented
_Complex float c = 1.0f + 2.0fi;      // ❌ Imaginary constants not implemented
```

**See also:**
- [Implementation Status](IMPLEMENTATION_STATUS.md) - Detailed status
- [Implementation Checklist](IMPLEMENTATION_CHECKLIST.md) - Task-by-task tracking

---

## Phase 0: Research and Preparation (RECOMMENDED)

**Goal:** Validate approach before major implementation.

### 0.1 Study Existing Implementations
- Examine GCC's complex handling: `gcc -fdump-tree-all test.c`
- Check Clang IR: `clang -S -emit-llvm test.c`
- Review ARM AAPCS §4.1.2 (composite types)

### 0.2 Verify ABI Compatibility
**Critical test:** Ensure TCC can call GCC-compiled complex functions.

```bash
# Compile with GCC
arm-none-eabi-gcc -c complex_func.c -o gcc_complex.o

# Call from TCC
./armv8m-tcc -c test_caller.c -o tcc_caller.o
arm-none-eabi-gcc tcc_caller.o gcc_complex.o -o test
```

### 0.3 Prototype struct-based approach
Test if lowering to struct early is viable:
```c
/* Quick prototype: map _Complex float to struct */
typedef struct { float __re; float __im; } __tcc_cfloat;
```
Compare code generation quality vs native approach.

### 0.4 Check TCC Type System Limits
```bash
# Find all VT_BTYPE users
grep -r "VT_BTYPE" *.c *.h | wc -l
# Estimate refactoring effort for mask expansion
```

**Deliverable:** Decision document: struct-based vs native complex types.

---

## Phase 1: Type System Foundation ✅ COMPLETE

**Goal:** Enable parsing and representation of complex types.

**Status:** All tasks completed. Type declarations, sizeof, and conversions work.

### 1.1 Add Complex Type Flag
**Files:** `tcc.h` ✅

**Decision Made:** Use `VT_COMPLEX` flag (bit 20) instead of expanding VT_BTYPE mask.

```c
/* Implementation: */
#define VT_COMPLEX  0x00100000   /* Complex type flag (bit 20) */
/* VT_FLOAT | VT_COMPLEX = float _Complex */
/* VT_DOUBLE | VT_COMPLEX = double _Complex */
```

**Rationale:** Avoids modifying core type mask, cleaner integration with existing code.

**Test:** `tests/ir_tests/50_complex_types.c` passes ✅

### 1.2 Update Parser Type Handling
**Files:** `tccgen.c` (parse_btype)

Replace the error with proper type handling:
```c
case TOK_COMPLEX:
    /* Mark that we saw _Complex, apply when float/double is seen */
    complex_flag = 1;
    next();
    break;
```

Then when `TOK_FLOAT` or `TOK_DOUBLE` is parsed, combine with complex flag:
```c
case TOK_FLOAT:
    if (complex_flag)
        u = VT_CFLOAT;
    else
        u = VT_FLOAT;
    goto basic_type;
```

### 1.3 Add Type Helper Functions
**Files:** `tcctype.h`

Add type checking utilities:
```c
static inline int tcc_is_complex_type(int t)
{
    int bt = t & VT_BTYPE;
    return (bt == VT_CFLOAT || bt == VT_CDOUBLE);
}

static inline int tcc_complex_base_type(int t)
{
    int bt = t & VT_BTYPE;
    if (bt == VT_CFLOAT) return VT_FLOAT;
    if (bt == VT_CDOUBLE) return VT_DOUBLE;
    return bt;
}
```

### 1.4 Update Type Size/Alignment Functions
**Files:** `tcctype.h`, `tccgen.c`

Update `tcc_get_basic_type_size()` and type alignment calculations:
```c
case VT_CFLOAT:
    return 8;   /* 2 floats */
case VT_CDOUBLE:
    return 16;  /* 2 doubles */
```

### 1.5 Type Conversion Rules
**Files:** `tccgen.c` (type conversion functions)

Implement C99 conversion rules:
```c
/* Real to complex: real part = value, imag = 0 */
float f = 1.0f;
_Complex float cf = f;  /* cf = 1.0 + 0i */

/* Complex to real: discard imaginary part (C99 6.3.1.7) */
_Complex float cf = 3.0f + 4.0fi;
float f = cf;  /* f = 3.0 (implicit conversion) */

/* Complex to complex: convert components */
_Complex float cf = 1.0f + 2.0fi;
_Complex double cd = cf;  /* widen both parts */

/* Integer to complex */
int x = 5;
_Complex float cf = x;  /* cf = 5.0 + 0i */
```

**Implementation:**
- Update `tcc_convert_type()` in `tccgen.c`
- Handle implicit conversions in assignments
- Handle explicit casts: `(_Complex float)expr`

### 1.6 Testing (Phase 1)
Create test file `tests/ir_tests/50_complex_types.c`:
```c
#include <stdio.h>

int main(void)
{
    _Complex float cf;
    _Complex double cd;
    
    /* Check sizes */
    if (sizeof(cf) != 8) return 1;
    if (sizeof(cd) != 16) return 1;
    
    printf("OK\n");
    return 0;
}
```

**Deliverable:** Parser accepts complex type declarations, sizeof works correctly.

---

## Phase 2: IR Support for Complex Types ✅ COMPLETE

**Goal:** Extend IR to represent complex values and operations.

**Status:** Complete. Complex types flow through IR with `is_complex` flag.

### 2.1 IROperand Complex Flag
**Files:** `tccir_operand.h`, `tccir_operand.c` ✅

Added `is_complex` field to `IROperand` struct:
```c
typedef struct IROperand {
    /* ... existing fields ... */
    int is_complex;   /* Set for complex float/double types */
} IROperand;
```

Functions updated:
- `svalue_to_iroperand()` - Sets `is_complex` from `VT_COMPLEX` flag
- `iroperand_to_svalue()` - Restores `VT_COMPLEX` flag

### 2.2 IR Operations Strategy
**Decision:** Lower complex operations to existing float ops in front-end.
- Complex add → Two float adds (real + real, imag + imag)
- Complex sub → Two float subtracts
- Complex mul/div → Component-wise operations (see Phase 3)

### 2.3 Testing (Phase 2)
Test IR dump shows correct complex types: `./armv8m-tcc -dump-ir -c test.c`

**Deliverable:** Complex types flow through IR with correct type information ✅

---

## Phase 3: Code Generation 🚧 PARTIAL

**Goal:** Generate ARM Thumb-2 code for complex operations.

**Status:** Add/Subtract implemented. **Multiplication and Division TODO.**

### 3.0 ARM AAPCS Calling Convention

**Software FP (no VFP):**
- `float _Complex`: Passed in r0 (real), r1 (imag); returned in r0, r1
- `double _Complex`: Passed in r0-r1 (real lo/hi), r2-r3 (imag lo/hi); returned same

**Hardware FP (VFP):**
- `float _Complex`: Passed in s0 (real), s1 (imag); returned in s0, s1
- `double _Complex`: Passed in d0 (real), d1 (imag); returned in d0, d1

### 3.1 Complex Number Representation ✅
Complex values use register pairs:
- `float _Complex`: rN (real), rN+1 (imag) or sN/sN+1 with VFP
- `double _Complex`: rN/rN+1 (real), rN+2/rN+3 (imag) or dN/dN+1 with VFP

### 3.2 Complex Load/Store ✅
**Files:** `arm-thumb-gen.c`

Load/store implemented via consecutive memory operations.

### 3.3 Complex Arithmetic Operations

#### Addition/Subtraction ✅
**Implementation:** `thumb_process_complex_op()` in `arm-thumb-gen.c`

Component-wise operations:
- Software FP: Calls `__addsf3`/`__subsf3` twice
- VFP: Inline VADD.F32/VSUB.F32

```c
/* float _Complex add: (a+ib) + (c+id) = (a+c) + i(b+d) */
VADD.F32 s0, s0, s2   /* real: a + c */
VADD.F32 s1, s1, s3   /* imag: b + d */
```

#### Multiplication ❌ TODO
**Formula:** `(a+ib) * (c+id) = (ac-bd) + i(ad+bc)`

**Implementation needed:**
```c
/* Software FP: Call runtime functions */
ac = __mulsf3(a, c);
bd = __mulsf3(b, d);
ad = __mulsf3(a, d);
bc = __mulsf3(b, c);
real = __subsf3(ac, bd);
imag = __addsf3(ad, bc);

/* VFP: Inline sequence */
VMUL.F32 s4, s0, s2    /* ac */
VMUL.F32 s5, s1, s3    /* bd */
VMUL.F32 s6, s0, s3    /* ad */
VMUL.F32 s7, s1, s2    /* bc */
VSUB.F32 s0, s4, s5    /* ac-bd (real) */
VADD.F32 s1, s6, s7    /* ad+bc (imag) */
```

#### Division ❌ TODO
**Formula:** `(ac+bd)/(c²+d²) + i(bc-ad)/(c²+d²)`

**Options:**
1. Inline expansion (many instructions)
2. Call runtime: `__divsc3` (float) / `__divdc3` (double)

**Recommendation:** Use runtime calls for software FP, inline for VFP.

### 3.4 Register Allocator ✅
**Files:** `tccls.c`

Register allocator handles complex values as pairs with consecutive registers.

### 3.5 Testing
- `tests/ir_tests/51_complex_arith.c` - Add/sub work ✅
- Multiplication tests - **Need implementation**
- Division tests - **Need implementation**

---

## Phase 4: Real and Imaginary Part Access

**Goal:** Support `__real__` and `__imag__` operators (GCC extension, widely used).

### 4.1 Add Keywords
**Files:** `tcctok.h`

```c
DEF(TOK_REAL, "__real__")
DEF(TOK_IMAG, "__imag__")
```

### 4.2 Parse Real/Imag Operators
**Files:** `tccgen.c`

Handle in expression parser:
```c
case TOK_REAL:
    next();
    parse_unary();  /* parse operand */
    /* Generate code to extract real part */
    if (tcc_is_complex_type(vtop->type.t)) {
        /* For float complex, just take lower 4 bytes */
        /* Mark as regular float type */
    }
    break;
```

### 4.3 Testing (Phase 4)
Test extraction and assignment to parts.

**Deliverable:** `__real__` and `__imag__` operators work.

---

## Phase 5: Complex Constants

**Goal:** Support imaginary constants like `1.0fi`, `2.0i`.

### 5.1 Add Imaginary Suffix Support
**Files:** `tccpp.c` (preprocessor number parsing)

Parse `i` or `j` suffix on floating constants (after `f` or no suffix).

### 5.2 Create Complex Constants
**Files:** `tccgen.c`

Generate constant complex values:
```c
/* 1.0fi -> {0.0f, 1.0f} */
/* Store in data section as two consecutive floats */
```

### 5.3 Testing (Phase 5)
Test constant initialization and usage.

**Deliverable:** Imaginary constants work correctly.

---

## Phase 6: Complex Built-in Functions

**Goal:** Provide `<complex.h>` library support.

### 6.1 Create complex.h Header
**Files:** `include/complex.h`

```c
#ifndef _COMPLEX_H
#define _COMPLEX_H

#define complex _Complex
#define _Complex_I 1.0fi
#define I _Complex_I

/* C11 CMPLX macros */
#define CMPLX(x, y) ((_Complex double){ x, y })
#define CMPLXF(x, y) ((_Complex float){ x, y })
#define CMPLXL(x, y) ((_Complex long double){ x, y })

/* Basic operations */
double creal(_Complex double z);
float crealf(_Complex float z);
/* ... etc ... */

#endif
```

### 6.2 Implement Complex Functions (Runtime)
**Files:** `lib/libtcc1.c` or link with newlib

Newlib already has complex math functions. Ensure ABI compatibility.

### 6.3 Testing (Phase 6)
Test against newlib's complex math functions.

**Deliverable:** `<complex.h>` usable, math functions work.

---

## Phase 7: Calling Conventions (ABI Compliance)

**Goal:** Ensure complex values are passed according to ARM AAPCS.

### 7.1 AAPCS Complex Calling Convention
According to AAPCS:
- `float _Complex`: passed in r0/r1 (or s0/s1 with VFP)
- `double _Complex`: passed in r0-r3 (or d0/d1 with VFP)
- Return values in same registers

### 7.2 Update Call Generation
**Files:** `arm-thumb-gen.c`, `tccir.c`

Ensure complex values are:
- Split into components for argument passing
- Recombined on function entry
- Properly returned

### 7.3 Testing (Phase 7)
Create `tests/ir_tests/52_complex_calls.c`:
```c
_Complex float add_complex(_Complex float a, _Complex float b)
{
    return a + b;
}

int main(void)
{
    _Complex float x = 1.0f + 2.0fi;
    _Complex float y = 3.0f + 4.0fi;
    _Complex float z = add_complex(x, y);
    /* Check result */
}
```

**Deliverable:** Complex values pass correctly across function calls.

---

## Phase 8: Debug Information

**Goal:** Generate correct DWARF debug info for complex types.

### 8.1 Update Debug Info Generation
**Files:** `tccdbg.c`

Add DWARF type entries for complex:
```c
case VT_CFLOAT:
    /* DW_ATE_complex_float with 8-byte size */
case VT_CDOUBLE:
    /* DW_ATE_complex_float with 16-byte size */
```

### 8.2 Testing (Phase 8)
Verify GDB can inspect complex variables.

**Deliverable:** Debug info correct, GDB shows complex values.

---

## Phase 9: Comprehensive Testing

### 9.1 Unit Tests
Create tests in `tests/ir_tests/`:

| Test | Description |
|------|-------------|
| `50_complex_types.c` | Type sizes, alignment |
| `51_complex_arith.c` | +, -, *, / operations |
| `52_complex_calls.c` | Function arguments/returns |
| `53_complex_real_imag.c` | `__real__`, `__imag__` |
| `54_complex_const.c` | Constant initialization |
| `55_complex_comparison.c` | ==, != operators |
| `56_complex_math.c` | cabs, cexp, etc. |

### 9.2 GCC Testsuite Integration
Identify relevant tests from `tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/`

### 9.3 Edge Cases
- Complex division by zero
- Complex NaN/Inf handling
- Mixed real/complex operations
- Complex bit-fields (should error)

---

## Implementation Order Summary

| Phase | Component | Effort | Priority |
|-------|-----------|--------|----------|
| 1 | Type System | Medium | Must have |
| 2 | IR Support | Low | Must have |
| 3 | Code Gen | High | Must have |
| 4 | Real/Imag Ops | Low | Should have |
| 5 | Constants | Medium | Should have |
| 6 | complex.h | Low | Should have |
| 7 | ABI/Calling | High | Must have |
| 8 | Debug Info | Low | Nice to have |
| 9 | Testing | High | Ongoing |

---

## Technical Notes

### Alternative: Lower to Struct Early
Instead of adding complex types throughout, could lower complex to a struct `{ T real; T imag; }` early in compilation. This would require less changes but lose type information for optimization.

### VFP vs Software FP
- With VFP: Use vector instructions for complex operations
- Software FP: Use integer register pairs and software FP library

### Complex Division
Complex division is the most complex operation. Options:
1. Inline the full calculation (many instructions)
2. Call runtime library function

Recommendation: Call runtime for software FP, inline for VFP.

---

## References

- C99 Standard, Section 7.3 (Complex arithmetic)
- ARM AAPCS, Section 4.3 (Parameter passing)
- GCC documentation on `_Complex` and `__real__`/`__imag__`
- Newlib complex.h implementation
