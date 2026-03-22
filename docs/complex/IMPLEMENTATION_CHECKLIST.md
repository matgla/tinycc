# Complex Number Support - Implementation Checklist

Use this checklist to track implementation progress.

## Legend
- [ ] Not started
- [-] In progress  
- [x] Complete

---

## Phase 0: Research and Preparation

### 0.1 ABI Research
- [x] Read ARM AAPCS §4.1.2 (composite types)
- [x] Study GCC complex handling: `gcc -fdump-tree-gimple test.c`
- [x] Study Clang LLVM IR: `clang -S -emit-llvm test.c`
- [x] Document exact register allocation for soft-float and VFP

### 0.2 VT_BTYPE Decision
- [x] Count all uses: `grep -r "VT_BTYPE" *.c *.h | wc -l`
- [x] Identify code that relies on mask being 0x000f
- [x] **Decision Made:** Use VT_COMPLEX flag (bit 20) instead of expanding mask
- [x] Document decision in DESIGN_DECISIONS.md

### 0.3 ABI Compatibility Test
- [-] Write GCC-compiled complex function
- [-] Call from TCC and verify result
- [ ] Test reverse direction (TCC → GCC call)
- [ ] Document any ABI incompatibilities

---

## Phase 1: Type System Foundation ✅ MOSTLY COMPLETE

### 1.1 Type Constants
- [x] Add `VT_COMPLEX` flag to `tcc.h` (bit 20, 0x00100000)
- [x] Verify no conflicts with other flags

### 1.2 Parser Changes
- [x] Modify `TOK_COMPLEX` handling in `parse_btype()` (`tccgen.c`)
- [x] Handle `float _Complex` -> `VT_FLOAT | VT_COMPLEX`
- [x] Handle `double _Complex` -> `VT_DOUBLE | VT_COMPLEX`
- [x] Handle `_Complex float` (reversed order)
- [x] Handle `_Complex double` (reversed order)
- [x] Handle `__complex__` GCC extension

### 1.3 Type Helper Functions
- [x] Add `tcc_is_complex_type()` to `tcctype.h`
- [x] Add `tcc_complex_base_type()` to `tcctype.h`
- [x] Add `tcc_is_complex_float()` helper
- [x] Add `tcc_is_complex_double()` helper

### 1.4 Type Size/Alignment
- [x] Update `tcc_get_basic_type_size()` for complex (8 for CFLOAT, 16 for CDOUBLE)
- [x] Verify alignment: 4-byte for CFLOAT, 8-byte for CDOUBLE
- [x] Check struct layout with complex members

### 1.5 Type Checking Updates
- [x] Find all `switch (bt)` on VT_BTYPE
- [x] Update type checking for VT_COMPLEX flag
- [x] Update `tcc_type_to_string()` for complex type names

### 1.6 Type Conversion Support
- [x] Update `tcc_convert_type()` for real → complex
- [x] Update `tcc_convert_type()` for complex → real (discard imag)
- [x] Update `tcc_convert_type()` for complex → complex (widen/narrow)
- [x] Update `tcc_convert_type()` for integer → complex
- [x] Implement explicit cast: `(_Complex float)expr`
- [-] Handle complex to bool conversion (C99 6.3.1.2)

### 1.7 Testing
- [x] Create `tests/ir_tests/50_complex_types.c`
- [x] Create `tests/ir_tests/50_complex_types.expect`
- [x] Test passes: `./run.py -c 50_complex_types.c`

---

## Phase 2: IR Support ✅ COMPLETE

### 2.1 IR Operand Type Encoding
- [x] Add `is_complex` field to `IROperand` in `tccir_operand.h`
- [x] Update encoding in `svalue_to_iroperand()`
- [x] Update decoding in `iroperand_to_svalue()`

### 2.2 IR Type Mapping
- [x] Ensure VT_COMPLEX flag maps to `is_complex` in IROperand
- [x] Ensure `is_complex` restores VT_COMPLEX flag

### 2.3 IR Dump Output
- [x] Verify `-dump-ir` shows correct complex types
- [x] Add type name for complex in IR debug output

### 2.4 Testing
- [x] Run `./armv8m-tcc -dump-ir -c test.c` and verify output

---

## Phase 3: Code Generation 🚧 PARTIAL

### 3.1 Complex Value Representation
- [x] Document register pair usage (r0/r1 for CFLOAT)
- [x] Document register quad usage (r0-r3 for CDOUBLE)
- [x] VFP register usage documented (s0/s1 for CFLOAT, d0/d1 for CDOUBLE)

### 3.2 Load Operations
- [x] Implement CFLOAT load (2 consecutive loads)
- [x] Implement CDOUBLE load (4 consecutive loads or 2 double loads)
- [x] Handle stack-based complex values

### 3.3 Store Operations
- [x] Implement CFLOAT store (2 consecutive stores)
- [x] Implement CDOUBLE store
- [x] Handle stack frame allocation for complex locals

### 3.4 Move Operations
- [x] Implement CFLOAT register-to-register move
- [x] Implement CDOUBLE register-to-register move

### 3.5 Addition/Subtraction
- [x] Software FP: CFLOAT add (call `__addsf3` x2)
- [x] Software FP: CDOUBLE add (call `__adddf3` x2)
- [x] `thumb_process_complex_op()` implemented

### 3.6 Multiplication
- [ ] Software FP: Call `__mulsf3` twice + `__subsf3` + `__addsf3`
- [ ] VFP: Inline VMUL + VSUB + VADD sequence
- [ ] Implement in `thumb_process_complex_op()` or new function

### 3.7 Division
- [ ] Software FP: Call `__divsc3`/`__divdc3` runtime function
- [ ] VFP: Implement inline or call runtime
- [ ] Handle edge cases (division by zero)

### 3.8 Negation
- [ ] Software FP: Negate both parts
- [ ] VFP: VNEG.F32/VNEG.F64 both parts

### 3.9 Register Allocator Updates
- [x] Ensure consecutive register allocation for complex
- [x] Handle spilling of complex values to stack
- [x] Update live range tracking for register pairs

### 3.10 Testing
- [-] Create `tests/ir_tests/51_complex_arith.c`
- [x] Addition test passes
- [x] Subtraction test passes
- [ ] Multiplication test passes
- [ ] Division test passes

---

## Phase 4: Real/Imaginary Accessors 🚧 PARTIAL

### 4.1 Keywords
- [x] Add `TOK_REAL` (`__real__`) to `tcctok.h`
- [x] Add `TOK_IMAG` (`__imag__`) to `tcctok.h`

### 4.2 Parser Support
- [x] Parse `__real__` unary expression
- [x] Parse `__imag__` unary expression
- [x] Generate code to extract real part
- [x] Generate code to extract imaginary part

### 4.3 L-value Support
- [ ] Allow `__real__ x = value;` (assignment)
- [ ] Allow `__imag__ x = value;` (assignment)
- [ ] Support address-of: `&__real__ x`

### 4.4 Testing
- [ ] Create `tests/ir_tests/53_complex_accessors.c`
- [ ] Read tests pass
- [ ] Write tests pass
- [ ] Address-of tests pass

---

## Phase 5: Complex Constants ❌ NOT STARTED

### 5.1 Lexer Changes
- [ ] Parse `i` suffix on float constants
- [ ] Parse `if` suffix (imaginary float)
- [ ] Parse `i` after regular float (e.g., `1.0i`)
- [ ] Handle `fi` suffix for float imaginary

### 5.2 Constant Creation
- [ ] Create zero real + imaginary value representation
- [ ] Store in data section
- [ ] Handle static initialization

### 5.3 _Complex_I Constant
- [ ] Ensure `_Complex_I` expands to `1.0fi` or similar
- [ ] Update `include/complex.h` if needed

### 5.4 Testing
- [ ] Create `tests/ir_tests/54_complex_init.c`
- [ ] Constant initialization tests pass
- [ ] Static initialization tests pass
- [ ] CMPLX macro works

---

## Phase 6: Complex Library Support ✅ COMPLETE

### 6.1 Header File
- [x] Create `include/complex.h`
- [x] Define `complex` macro to `_Complex`
- [x] Define `_Complex_I` (placeholder until constants work)
- [x] Define `I`
- [x] Add CMPLX/CMPLXF/CMPLXL macros

### 6.2 Basic Functions
- [x] `creal/crealf/creall` (inline implementations)
- [x] `cimag/cimagf/cimagl` (inline implementations)
- [x] `conj/conjf/conjl` (link to newlib)
- [x] `cabs/cabsf/cabsl` (link to newlib)

### 6.3 Math Functions
- [x] All math functions link to newlib

### 6.4 Testing
- [ ] Create `tests/ir_tests/57_complex_math.c`
- [ ] Basic function tests pass
- [ ] Math function tests pass

---

## Phase 7: Calling Conventions 🚧 PARTIAL

### 7.1 Parameter Passing
- [x] CFLOAT in r0/r1 (soft float) or s0/s1 (VFP)
- [x] CDOUBLE in r0-r3 (soft float) or d0/d1 (VFP)
- [ ] Stack parameter passing for overflow (verify)

### 7.2 Return Values
- [x] CFLOAT return in r0/r1 or s0/s1
- [x] CDOUBLE return in r0-r3 or d0/d1

### 7.3 Function Prologue/Epilogue
- [x] Correct stack frame for complex locals
- [x] Save/restore complex callee-saved registers

### 7.4 Varargs (Optional)
- [ ] Decide if complex in varargs supported
- [ ] Document limitation if not supported

### 7.5 Testing
- [ ] Create `tests/ir_tests/52_complex_calls.c`
- [ ] Pass by value tests pass
- [ ] Return value tests pass
- [ ] Nested call tests pass

---

## Phase 8: Debug Information ❌ NOT STARTED

### 8.1 DWARF Types
- [ ] Add DWARF type entry for CFLOAT
- [ ] Add DWARF type entry for CDOUBLE
- [ ] Use DW_ATE_complex_float

### 8.2 Debug Output
- [ ] Verify `tccdbg.c` handles VT_COMPLEX
- [ ] Verify correct debug info generation

### 8.3 Testing
- [ ] Compile with `-g`
- [ ] Verify GDB can inspect complex variables
- [ ] Verify correct values shown in debugger

---

## Phase 9: Testing & Quality 🚧 IN PROGRESS

### 9.1 Unit Tests
- [x] 50_complex_types.c passes
- [-] 51_complex_arith.c (add/sub only)
- [ ] 52_complex_calls.c
- [ ] 53_complex_accessors.c
- [ ] 54_complex_init.c
- [ ] 55_complex_compare.c
- [ ] 56_complex_edge.c
- [ ] 57_complex_math.c

### 9.2 Negative Tests
- [ ] Complex bit-field produces error
- [ ] Ordered comparison produces error
- [ ] Clear error messages

### 9.3 GCC Testsuite
- [ ] Identify relevant GCC tests
- [ ] Run GCC complex tests
- [ ] Document pass/fail status

### 9.4 Regression Testing
- [-] Run full test suite: `make test -j16`
- [x] No regressions in existing tests (verified for Phases 1-2)

### 9.5 Code Review
- [ ] Review all changes
- [ ] Check for code style compliance
- [ ] Verify comments added

---

## Quick Reference: Current Status

| Phase | Status | % Complete |
|-------|--------|------------|
| 0: Research | ✅ Done | 100% |
| 1: Type System | ✅ Done | 95% |
| 2: IR Support | ✅ Done | 100% |
| 3: Code Gen | 🚧 Partial | 50% |
| 4: Accessors | 🚧 Partial | 60% |
| 5: Constants | ❌ Not Started | 0% |
| 6: Library | ✅ Done | 90% |
| 7: Calling Conv | 🚧 Partial | 70% |
| 8: Debug Info | ❌ Not Started | 0% |
| 9: Testing | 🚧 In Progress | 30% |

**Overall Completion: ~60%**

---

## Next Actions (Recommended Priority)

1. **Implement Complex Multiplication** (Phase 3) - High Impact
2. **Implement Complex Division** (Phase 3) - High Impact  
3. **Add Imaginary Constant Support** (Phase 5) - High Impact
4. **Create Missing Test Files** (Phase 9) - Medium Impact
5. **Complete __real__/__imag__ L-values** (Phase 4) - Medium Impact
