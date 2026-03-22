# Complex Number Support - Implementation Status

**Last Updated:** 2026-02-26

## Summary

Complex number support in TinyCC for ARMv8-M is **partially implemented**. Phase 1 (Type System) and Phase 2 (IR Support) are functionally complete. Phase 3 (Code Generation) has basic arithmetic working but needs completion for full compliance.

**Recent Changes:** Implemented fixes from FIX_PLAN.md - corrected register allocation for complex parameters and IR generation for FMUL/FDIV.

## Implementation Progress by Phase

### Phase 1: Type System Foundation ✅ COMPLETE

| Component | Status | Notes |
|-----------|--------|-------|
| VT_COMPLEX flag | ✅ Done | Implemented as bit 20 flag (0x00100000) |
| Parser (`TOK_COMPLEX`) | ✅ Done | `parse_btype()` handles `_Complex` keyword |
| Type helpers | ✅ Done | `tcc_is_complex_type()` etc. in `tcctype.h` |
| Size/alignment | ✅ Done | 8 bytes for CFLOAT, 16 for CDOUBLE |
| Type conversions | ✅ Done | Real↔Complex, widening, casting |
| `__real__`/`__imag__` | ✅ Partial | Parser recognizes, basic implementation |

**Files Modified:**
- `tcc.h` - Added `VT_COMPLEX` flag
- `tcctok.h` - Added `TOK_REAL`, `TOK_IMAG`
- `tcctype.h` - Added complex type helper functions
- `tccgen.c` - Parser changes for complex types

**Test Status:** `tests/ir_tests/50_complex_types.c` ✅ PASSES

---

### Phase 2: IR Support ✅ COMPLETE

| Component | Status | Notes |
|-----------|--------|-------|
| IROperand complex flag | ✅ Done | `is_complex` field added |
| Type encoding | ✅ Done | `svalue_to_iroperand()` handles complex |
| Type decoding | ✅ Done | `iroperand_to_svalue()` restores complex flag |
| IR dump output | ✅ Done | Shows complex types correctly |

**Files Modified:**
- `tccir_operand.h` - Added `is_complex` field to `IROperand`
- `tccir_operand.c` - Encoding/decoding logic for complex types

**Test Status:** `./armv8m-tcc -dump-ir` shows correct complex types ✅

---

### Phase 3: Code Generation 🚧 PARTIAL (Fixes Applied)

| Component | Status | Notes |
|-----------|--------|-------|
| Value representation | ✅ Done | Register pairs for complex values |
| Load/store | ✅ Done | Consecutive memory operations |
| Addition/Subtraction | ✅ Done | `thumb_process_complex_op()` implemented |
| Multiplication | 🚧 Fixed | Rewritten with stack-based approach |
| Division | 🚧 Fixed | Uses `__divsc3` runtime call |
| Register allocator | ✅ Done | Handles register pairs |

**Fixes Applied (from FIX_PLAN.md):**

1. ✅ **Fix 1:** Mark param/var vregs as complex (`tccgen.c:805-807, 832-834`)
2. ✅ **Fix 2:** Fix incoming register assignment (`ir/codegen.c:365`) - added `is_complex` check
3. ⏭️ **Fix 3:** Handle real-to-complex initialization - NOT YET DONE
4. ✅ **Fix 4:** Fix stack corruption in `thumb_process_complex_op` - removed extra SP adjustment
5. ✅ **Fix 5:** Add FMUL/FDIV to complex IR generation (`ir/core.c:1168`)
6. ✅ **Fix 6:** Rewrite `thumb_process_complex_mul` with stack-based approach
7. ✅ **Fix 7:** Fix register ordering in `thumb_process_complex_div`
8. ⏭️ **Fix 8:** Remove debug fprintf statements - NOT YET DONE

**Files Modified:**
- `arm-thumb-gen.c` - Complex operation handling
- `ir/codegen.c` - Register assignment for complex params
- `ir/core.c` - FMUL/FDIV IR generation

**Known Issues:**
- Complex multiplication/division still cause HardFault at runtime - needs further debugging
- Debug output still enabled (`DEBUG` macros active)

---

### Phase 4: Real/Imaginary Accessors 🚧 PARTIAL

| Component | Status | Notes |
|-----------|--------|-------|
| Keywords | ✅ Done | `TOK_REAL`, `TOK_IMAG` in `tcctok.h` |
| Parser | ✅ Done | Unary expression parsing |
| Code generation | ✅ Basic | Extraction works |
| L-value support | ❌ TODO | Assignment to `__real__ x` not complete |
| Address-of | ❌ TODO | `&__real__ x` not complete |

**Files Modified:**
- `tcctok.h` - Token definitions
- `tccgen.c` - Parser support (lines 7097-7120)

---

### Phase 5: Complex Constants ❌ NOT STARTED

| Component | Status | Notes |
|-----------|--------|-------|
| Imaginary suffix | ❌ TODO | `1.0fi`, `2.0i` parsing |
| Constant creation | ❌ TODO | Data section storage |
| `_Complex_I` | ❌ TODO | Macro definition |

**Blocker:** Lexer changes needed in `tccpp.c` for imaginary suffix parsing.

---

### Phase 6: Complex Library Support 🚧 PARTIAL

| Component | Status | Notes |
|-----------|--------|-------|
| `complex.h` header | ✅ Done | `include/complex.h` created |
| `complex` macro | ✅ Done | Maps to `_Complex` |
| `I` macro | ⚠️ Partial | Defined but `1.0fi` not working yet |
| `CMPLX` macros | ✅ Done | Compound literal versions |
| `creal/cimag` | ✅ Done | Inline implementations |
| Math functions | ✅ Deferred | Using newlib's implementations |

**Files Created:**
- `include/complex.h` - C99 complex header (complete)

---

### Phase 7: Calling Conventions 🚧 PARTIAL

| Component | Status | Notes |
|-----------|--------|-------|
| Parameter passing | ✅ Basic | Works for simple cases |
| Return values | ✅ Basic | Works for simple cases |
| AAPCS compliance | ⚠️ Review needed | Verify against spec |
| Stack overflow | ❌ TODO | Complex on stack |
| Varargs | ❌ Deferred | Low priority |

**Files Modified:**
- `arm-thumb-gen.c` - Call site handling
- `arm-thumb-callsite.c` - Argument passing

---

### Phase 8: Debug Information ❌ NOT STARTED

| Component | Status | Notes |
|-----------|--------|-------|
| DWARF types | ❌ TODO | Add complex float/double entries |
| GDB testing | ❌ TODO | Verify variable inspection |

**Files to Modify:**
- `tccdbg.c` - Debug info generation

---

### Phase 9: Testing 🚧 IN PROGRESS

| Test | Status |
|------|--------|
| `50_complex_types.c` | ✅ PASS |
| `51_complex_arith.c` | 🚧 Partial (add/sub only, mul/div need debugging) |
| `52_complex_calls.c` | ❌ Not created |
| `53_complex_accessors.c` | ❌ Not created |
| `54_complex_init.c` | ❌ Not created |
| `55_complex_compare.c` | ❌ Not created |
| `56_complex_edge.c` | ❌ Not created |
| `57_complex_math.c` | ❌ Not created |

---

## What Works Now

### ✅ Type Declarations
```c
_Complex float cf;
_Complex double cd;
float _Complex cf2;  /* Alternate syntax */
```

### ✅ sizeof
```c
sizeof(_Complex float)    /* Returns 8 */
sizeof(_Complex double)   /* Returns 16 */
```

### ✅ Basic Arithmetic (Add/Subtract)
```c
_Complex float a = ...;
_Complex float b = ...;
_Complex float c = a + b;  /* Works */
_Complex float d = a - b;  /* Works */
```

### ✅ Type Conversions
```c
float f = 3.0f;
_Complex float cf = f;     /* Real -> Complex */
float g = cf;              /* Complex -> Real (discards imag) */
```

### ✅ complex.h Header
```c
#include <complex.h>
complex double z;          /* 'complex' macro works */
```

---

## What's Missing / Not Working

### ❌ Complex Multiplication and Division (Partially Fixed)
```c
_Complex float c = a * b;  /* Code generation rewritten but still HardFaults */
_Complex float d = a / b;  /* Code generation rewritten but still HardFaults */
```

**Status:** Applied fixes from FIX_PLAN.md, but runtime issues remain.

### ❌ Imaginary Constants
```c
_Complex float c = 1.0f + 2.0fi;  /* ERROR: 'fi' suffix not recognized */
```

### ❌ Full __real__/__imag__ L-value Support
```c
__real__ c = 5.0f;   /* May not work */
&__real__ c;         /* May not work */
```

---

## Next Steps (Priority Order)

### High Priority
1. **Debug Complex Multiplication/Division** - The stack-based implementations are in place but still causing HardFaults. Need to debug the generated assembly.
2. **Remove Debug Output** - Clean up all DEBUG fprintf statements

### Medium Priority
3. **Imaginary Constant Support** - Add `fi`/`i` suffix parsing in `tccpp.c`
4. **Complete __real__/__imag__ L-value Support**
5. **Create Missing Test Files** - Tests 52-57

### Low Priority
6. **Debug Information** (Phase 8)
7. **Varargs Support** (Phase 7)
8. **Complex Integer Types** (GCC extension)

---

## Testing Commands

```bash
# Type system test
cd tests/ir_tests
python run.py -c 50_complex_types.c

# Check IR output
./armv8m-tcc -dump-ir -c test.c

# Compile complex test
./armv8m-tcc -c test_complex.c -o test_complex.o
```

---

## References

- Original Plan: `README.md`
- Design Decisions: `DESIGN_DECISIONS.md`
- Test Plan: `TEST_PLAN.md`
- Getting Started: `GETTING_STARTED.md`
- Fix Plan: `FIX_PLAN.md`
