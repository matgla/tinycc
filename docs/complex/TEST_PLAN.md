# Complex Number Support - Test Plan

## Overview

This document defines comprehensive testing for complex number support. Tests are organized by phase and include positive tests, negative tests, and edge cases.

## Test Organization

```
tests/ir_tests/
├── 50_complex_types.c          # Phase 1: Type system tests
├── 50_complex_types.expect
├── 51_complex_arith.c          # Phase 3: Arithmetic operations
├── 51_complex_arith.expect
├── 52_complex_calls.c          # Phase 7: Function calls
├── 52_complex_calls.expect
├── 53_complex_accessors.c      # Phase 4: __real__, __imag__
├── 53_complex_accessors.expect
├── 54_complex_init.c           # Phase 5: Initialization
├── 54_complex_init.expect
├── 55_complex_compare.c        # Equality comparison
├── 55_complex_compare.expect
├── 56_complex_edge.c           # Edge cases
├── 56_complex_edge.expect
└── 57_complex_math.c           # Phase 6: Math functions
    └── 57_complex_math.expect
```

## Phase 1: Type System Tests (50_complex_types.c)

### Test 1.1: Size and Alignment
```c
#include <stdio.h>

int main(void)
{
    printf("sizeof(float) = %d\n", (int)sizeof(float));
    printf("sizeof(double) = %d\n", (int)sizeof(double));
    printf("sizeof(float _Complex) = %d\n", (int)sizeof(float _Complex));
    printf("sizeof(double _Complex) = %d\n", (int)sizeof(double _Complex));
    printf("sizeof(long double _Complex) = %d\n", (int)sizeof(long double _Complex));
    return 0;
}
```

**Expected output:**
```
sizeof(float) = 4
sizeof(double) = 8
sizeof(float _Complex) = 8
sizeof(double _Complex) = 16
sizeof(long double _Complex) = 16
```

### Test 1.2: Type Declaration Variations
```c
_Complex float cf1;
float _Complex cf2;
_Complex double cd1;
double _Complex cd2;
__complex__ float gcf;    /* GCC extension */
```

### Test 1.3: Array of Complex
```c
_Complex float arr[10];
printf("sizeof(arr) = %d\n", (int)sizeof(arr));  /* Should be 80 */
```

### Test 1.4: Pointer to Complex
```c
_Complex float *p;
printf("sizeof(p) = %d\n", (int)sizeof(p));  /* Should be 4 (pointer) */
```

### Test 1.5: Complex Struct Member
```c
struct S {
    _Complex float c;
    int x;
};
printf("sizeof(struct S) = %d\n", (int)sizeof(struct S));  /* Should be 16 (8 + 4 + 4 pad) */
```

---

## Phase 3: Arithmetic Tests (51_complex_arith.c)

### Test 3.1: Complex Addition
```c
_Complex float a = 1.0f + 2.0fi;
_Complex float b = 3.0f + 4.0fi;
_Complex float c = a + b;
printf("%.1f %.1f\n", __real__ c, __imag__ c);  /* "4.0 6.0" */
```

### Test 3.2: Complex Subtraction
```c
_Complex float c = a - b;
printf("%.1f %.1f\n", __real__ c, __imag__ c);  /* "-2.0 -2.0" */
```

### Test 3.3: Complex Multiplication
```c
/* (1+2i) * (3+4i) = (3-8) + i(4+6) = -5 + 10i */
_Complex float c = a * b;
printf("%.1f %.1f\n", __real__ c, __imag__ c);  /* "-5.0 10.0" */
```

### Test 3.4: Complex Division
```c
/* (5+10i) / (1+2i) = 5 */
_Complex float num = 5.0f + 10.0fi;
_Complex float den = 1.0f + 2.0fi;
_Complex float quot = num / den;
printf("%.1f %.1f\n", __real__ quot, __imag__ quot);  /* "5.0 0.0" */
```

### Test 3.5: Double Complex Operations
Same tests with `double _Complex` to verify 16-byte operations.

### Test 3.6: Mixed Real and Complex
```c
_Complex float c = a + 5.0f;  /* 5 is real, should add to real part */
printf("%.1f %.1f\n", __real__ c, __imag__ c);  /* "6.0 2.0" */
```

### Test 3.7: Complex Negation
```c
_Complex float c = -a;
printf("%.1f %.1f\n", __real__ c, __imag__ c);  /* "-1.0 -2.0" */
```

---

## Phase 4: Accessor Tests (53_complex_accessors.c)

### Test 4.1: Read Real and Imaginary
```c
_Complex float c = 3.0f + 4.0fi;
float r = __real__ c;
float i = __imag__ c;
printf("%.1f %.1f\n", r, i);  /* "3.0 4.0" */
```

### Test 4.2: Modify Real Part
```c
_Complex float c = 3.0f + 4.0fi;
__real__ c = 10.0f;
printf("%.1f %.1f\n", __real__ c, __imag__ c);  /* "10.0 4.0" */
```

### Test 4.3: Modify Imaginary Part
```c
_Complex float c = 3.0f + 4.0fi;
__imag__ c = 20.0f;
printf("%.1f %.1f\n", __real__ c, __imag__ c);  /* "3.0 20.0" */
```

### Test 4.4: Address of Parts
```c
_Complex float c = 3.0f + 4.0fi;
float *rp = &__real__ c;
float *ip = &__imag__ c;
*rp = 100.0f;
printf("%.1f\n", __real__ c);  /* "100.0" */
```

---

## Phase 5: Initialization Tests (54_complex_init.c)

### Test 5.1: Compound Literal Initialization
```c
_Complex float c = 1.0f + 2.0fi;
```

### Test 5.2: Real-Only Initialization
```c
_Complex float c = 5.0f;  /* Imaginary part is 0 */
printf("%.1f %.1f\n", __real__ c, __imag__ c);  /* "5.0 0.0" */
```

### Test 5.3: CMPLX Macro
```c
#include <complex.h>
_Complex float c = CMPLXF(1.0f, 2.0f);
```

### Test 5.4: Static Initialization
```c
static _Complex float c = 1.0f + 2.0fi;
```

### Test 5.5: Array Initialization
```c
_Complex float arr[3] = {1.0f, 2.0f + 3.0fi, 4.0f};
```

---

## Phase 7: Function Call Tests (52_complex_calls.c)

### Test 7.1: Pass and Return Complex
```c
_Complex float add(_Complex float a, _Complex float b)
{
    return a + b;
}

int main(void)
{
    _Complex float x = 1.0f + 2.0fi;
    _Complex float y = 3.0f + 4.0fi;
    _Complex float z = add(x, y);
    printf("%.1f %.1f\n", __real__ z, __imag__ z);  /* "4.0 6.0" */
    return 0;
}
```

### Test 7.2: Complex in Struct Parameter
```c
struct Pair {
    _Complex float c;
    int n;
};

void process(struct Pair p);
```

### Test 7.3: Complex Variadic Functions (if supported)
```c
/* Note: complex in varargs may have special requirements */
```

---

## Comparison Tests (55_complex_compare.c)

### Test 5.1: Equality
```c
_Complex float a = 1.0f + 2.0fi;
_Complex float b = 1.0f + 2.0fi;
_Complex float c = 3.0f + 4.0fi;
printf("%d %d\n", a == b, a == c);  /* "1 0" */
```

### Test 5.2: Inequality
```c
printf("%d %d\n", a != b, a != c);  /* "0 1" */
```

### Test 5.3: Ordered Comparison (Compile Error Test)
```c
/* This should produce compile error */
if (a < b) { }  /* error: invalid operands to binary < */
```

---

## Edge Case Tests (56_complex_edge.c)

### Test 6.1: Division by Zero
```c
_Complex float a = 1.0f + 2.0fi;
_Complex float zero = 0.0f + 0.0fi;
_Complex float c = a / zero;
/* Should produce Inf or NaN */
```

### Test 6.2: NaN Propagation
```c
/* Operations with NaN should produce NaN */
```

### Test 6.3: Infinity
```c
/* Operations with Inf should follow IEEE rules */
```

### Test 6.4: Very Large/Small Numbers
```c
/* Test for overflow/underflow */
```

### Test 6.5: Pure Real/Pure Imaginary
```c
_Complex float real_only = 5.0f;        /* 5 + 0i */
_Complex float imag_only = 5.0fi;       /* 0 + 5i */
```

---

## Math Library Tests (57_complex_math.c)

### Test 7.1: cabs (Absolute Value)
```c
#include <complex.h>
_Complex float c = 3.0f + 4.0fi;
float a = cabsf(c);
printf("%.1f\n", a);  /* "5.0" */
```

### Test 7.2: creal/cimag
```c
_Complex float c = 3.0f + 4.0fi;
printf("%.1f %.1f\n", crealf(c), cimagf(c));  /* "3.0 4.0" */
```

### Test 7.3: conj (Conjugate)
```c
_Complex float c = 3.0f + 4.0fi;
_Complex float conj_c = conjf(c);
printf("%.1f %.1f\n", __real__ conj_c, __imag__ conj_c);  /* "3.0 -4.0" */
```

### Test 7.4: cexp
```c
/* e^(0 + i*pi) = -1 */
_Complex float c = cexpf(0.0f + 3.14159265fi);
/* Should be approximately -1 + 0i */
```

### Test 7.5: csqrt
```c
/* sqrt(-1) = i */
_Complex float c = csqrtf(-1.0f + 0.0fi);
/* Should be approximately 0 + 1i */
```

---

## Type Conversion Tests (NEW)

### TConv 1: Real to Complex
```c
float f = 3.0f;
_Complex float cf = f;
printf("%.1f %.1f\n", __real__ cf, __imag__ cf);  /* "3.0 0.0" */
```

### TConv 2: Complex to Real (Implicit)
```c
_Complex float cf = 3.0f + 4.0fi;
float f = cf;  /* Discard imaginary part */
printf("%.1f\n", f);  /* "3.0" */
```

### TConv 3: Complex Widening
```c
_Complex float cf = 1.0f + 2.0fi;
_Complex double cd = cf;  /* Widen both components */
```

### TConv 4: Integer to Complex
```c
int x = 5;
_Complex float cf = x;
printf("%.1f %.1f\n", __real__ cf, __imag__ cf);  /* "5.0 0.0" */
```

### TConv 5: Cast Operations
```c
_Complex double cd = (_Complex double)(3.0f + 4.0fi);
float f = (float)(5.0 + 10.0i);  /* f = 5.0 */
```

---

## ABI Compatibility Tests (NEW - CRITICAL)

### ABI 1: Call GCC-Compiled Function
```c
/* gcc_func.c - compiled with arm-none-eabi-gcc */
_Complex float gcc_add(_Complex float a, _Complex float b)
{
    return a + b;
}

/* tcc_caller.c - compiled with TCC */
extern _Complex float gcc_add(_Complex float, _Complex float);

int main(void)
{
    _Complex float x = 1.0f + 2.0fi;
    _Complex float y = 3.0f + 4.0fi;
    _Complex float z = gcc_add(x, y);
    /* Verify result correct */
}
```

### ABI 2: TCC Function Called by GCC
Reverse of ABI 1 - TCC implements, GCC calls.

### ABI 3: Stack Parameter Passing
```c
/* Force parameters onto stack */
void many_params(
    int a, int b, int c, int d,  /* Use r0-r3 */
    _Complex float cf);          /* Must go on stack */
```

---

## Union and Aliasing Tests (NEW)

### Union 1: Complex in Union
```c
union U {
    _Complex float cf;
    float arr[2];
};
union U u;
u.cf = 1.0f + 2.0fi;
printf("%.1f %.1f\n", u.arr[0], u.arr[1]);  /* "1.0 2.0" */
```

### Union 2: Pointer Aliasing
```c
_Complex float cf = 3.0f + 4.0fi;
float *fp = (float *)&cf;
printf("%.1f %.1f\n", fp[0], fp[1]);  /* "3.0 4.0" */
```

---

## Negative Tests (Should Produce Errors)

### NTest 1: Complex Bit-field
```c
struct S {
    _Complex int x : 8;  /* error: bit-field has invalid type */
};
```

### NTest 2: Ordered Comparison
```c
_Complex float a, b;
if (a < b) { }  /* error: invalid operands to binary < */
```

### NTest 3: Complex Integer (if not supported)
```c
_Complex int x;  /* may be error or warning */
```

### NTest 4: Cast to Complex Integer
```c
int x = 5;
_Complex int c = (_Complex int)x;  /* error if not supported */
```

---

## GCC Testsuite Integration

Relevant tests from GCC c-torture suite:

```
tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/
├── compile/
│   └── complex/    (if exists)
└── execute/
    └── complex/    (if exists)
```

Also check:
```
tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.dg/complex*
```

---

## Test Automation

### Running Tests
```bash
# Individual test
cd tests/ir_tests
python run.py -c 50_complex_types.c

# All complex tests
pytest -k "complex" -v

# Full test suite (after full implementation)
make test -j16
```

### Expected Files Format
Each `.expect` file contains expected stdout output:
```
sizeof(float) = 4
sizeof(double) = 8
sizeof(float _Complex) = 8
sizeof(double _Complex) = 16
OK
```

---

## Success Criteria

| Phase | Pass Criteria |
|-------|--------------|
| 1 | All type tests pass, sizeof correct |
| 2 | IR dump shows correct complex types |
| 3 | Arithmetic tests within 0.0001 tolerance |
| 4 | Accessor tests pass |
| 5 | Initialization tests pass |
| 6 | complex.h usable, basic functions work |
| 7 | Function call tests pass |
| 8 | Debug info valid (GDB check) |
| 9 | All tests pass, no regressions |

---

## Performance Benchmarks (Future)

Once basic functionality works, consider:

1. **FFT benchmark:** Compare TCC vs GCC for DFT/FFT algorithms
2. **Matrix multiply:** Complex matrix operations
3. **Filter banks:** Digital signal processing kernels
