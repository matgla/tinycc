# Complex Number Support - Design Decisions

This document records key design decisions for the complex number implementation.

## Decision 1: Type Representation

### Option A: New VT_BTYPE values
Add `VT_CFLOAT` (15) and `VT_CDOUBLE` (16) as new basic types.

**Pros:**
- Clean separation of complex types
- Easy type checking with simple bit tests
- Follows pattern of other fundamental types

**Cons:**
- Requires changing VT_BTYPE mask if we exceed 16 types
- Need to update all switch statements on VT_BTYPE

### Option B: VT_COMPLEX flag
Add a `VT_COMPLEX` flag bit that combines with `VT_FLOAT`/`VT_DOUBLE`.

**Pros:**
- No new basic types needed
- Natural composition of properties

**Cons:**
- More complex type checking logic everywhere
- May conflict with existing flag bits

### Decision: Option A (New VT_BTYPE values)
**Rationale:** Complex types are distinct fundamental types in C99. The explicit approach is cleaner and less error-prone.

**CRITICAL REQUIREMENT:** Must expand VT_BTYPE mask from 0x000f to 0x001f (4 bits → 5 bits) to accommodate VT_CDOUBLE = 16.

**Implementation steps:**
1. Change `#define VT_BTYPE 0x000f` to `0x001f` in `tcc.h`
2. Audit all code that uses VT_BTYPE (estimated ~50-100 locations)
3. Verify no conflicts with other flag bits (VT_UNSIGNED, VT_ARRAY, etc.)
4. Run full test suite to catch regressions

**Alternative if mask expansion too risky:** Fall back to Option B (VT_COMPLEX flag)

---

## Decision 2: IR Representation

### Option A: Native complex operations
Add `TCCIR_OP_CADD`, `TCCIR_OP_CMUL`, etc.

**Pros:**
- Backend can optimize complex operations
- Cleaner IR representation

**Cons:**
- More IR opcodes to implement in backend
- Optimization passes need to understand complex semantics

### Option B: Lower to scalar operations
Complex `a + b` becomes operations on real and imag parts separately.

**Pros:**
- Reuses existing IR operations
- No new opcodes needed
- Optimization passes work automatically

**Cons:**
- Loses semantic information early
- Backend can't optimize as effectively

### Decision: Option B (Lower to scalar operations)
**Rationale:** Simpler implementation, leverages existing optimizer. Can revisit if complex optimization becomes critical.

---

## Decision 3: Register Allocation

### Option A: Treat as 64/128-bit value
Use 2 or 4 registers as a single unit.

**Pros:**
- Natural for moves and copies
- Consistent with struct passing

**Cons:**
- Register allocator needs to reserve consecutive registers
- Complex to handle spilling

### Option B: Split into real/imag components
Allocate separate vregs for real and imaginary parts.

**Pros:**
- Simpler register allocation
- Better register utilization

**Cons:**
- More vregs created
- Need to track pairing

### Decision: Option A (Treat as unit)
**Rationale:** Aligns with AAPCS which treats complex as unit. Simpler code generation.

---

## Decision 4: Complex Division Implementation

### Option A: Inline expansion
Generate full instruction sequence for division.

**Pros:**
- No function call overhead
- Better for optimization

**Cons:**
- Many instructions (~20+ for software FP)
- Code bloat

### Option B: Runtime library call
Call `__divsc3` (float) or `__divdc3` (double).

**Pros:**
- Smaller code
- Library handles edge cases (NaN, Inf)

**Cons:**
- Function call overhead
- Dependency on libgcc or libtcc1

### Decision: Hybrid approach
- **VFP targets:** Inline for float complex, call runtime for double complex
- **Software FP:** Always call runtime

---

## Decision 5: `__real__` and `__imag__` Support

### Option A: GCC extensions only
Support only when `-std=gnu99` or extensions enabled.

### Option B: Always support
Treat as always available (like GCC does).

### Decision: Option B (Always support)
**Rationale:** These operators are essential for complex number programming and widely expected. Newlib's complex.h relies on them.

---

## Decision 6: Complex Constants

### Option A: Native lexer support
Parse `1.0fi` directly in lexer.

**Pros:**
- Cleaner
- Better error messages

**Cons:**
- More lexer changes

### Option B: Preprocessor macro
Define `__fic(x)` macro that constructs complex.

**Pros:**
- Simpler implementation

**Cons:**
- Doesn't match user expectations
- Won't work with newlib's `I` macro

### Decision: Option A (Native support)
**Rationale:** The `1.0fi` syntax is standard C99. Must support directly.

---

## Decision 7: Complex Comparison Operators

C99 specifies that complex types only support `==` and `!=` (equality comparison).

### Decision: Follow C99 strictly
- `==` and `!=` : Compare both real and imaginary parts
- `<`, `>`, `<=`, `>=` : Compile error

**Note:** May need special handling in parser to give clear error for ordered comparison of complex.

---

## Decision 8: VFP vs Software FP Code Paths

### Decision: Conditional code generation in arm-thumb-gen.c

```c
if (arch_config->has_fpu) {
    /* Generate VFP instructions */
} else {
    /* Call runtime functions or use integer ops */
}
```

The runtime functions (e.g., `__addsf3`, `__mulsf3`) are already provided by libtcc1 or newlib.

---

## Open Questions

1. **Struct-based vs Native Implementation:** Should we reconsider lowering `_Complex float` to `struct { float __re; float __im; }` early in compilation? This would:
   - Reuse all existing struct handling (ABI, codegen, etc.)
   - Require minimal type system changes
   - Lose some type information for diagnostics
   - Need special-case handling for `__real__`/`__imag__`

   **Recommendation:** Prototype both approaches in Phase 0 and measure implementation effort.

2. **VT_BTYPE mask expansion risk:** Expanding from 0x000f to 0x001f affects core type system. What's the blast radius?
   - How many places use VT_BTYPE?
   - Do any flags rely on bit 4 being available?
   - Performance impact of 5-bit vs 4-bit mask?

3. **Long double complex:** On ARM, `long double` is same as `double`. Should `long double complex` be:
   - Same as `double complex` (same VT_CDOUBLE)
   - Distinct type (new VT_CLDOUBLE = VT_CDOUBLE alias)

   **Recommendation:** Same type, simpler implementation.

4. **Complex integers:** C99 doesn't support `_Complex int`, but GCC has extension. Should we support it?
   - **Phase 1:** Reject with clear error
   - **Future:** Add if users request

5. **Complex bit-fields:** GCC rejects these. We should too, but when? Parse time or later?
   **Recommendation:** Parse time, clearer error message.

6. **Type-generic math:** `<tgmath.h>` macros need to dispatch to complex functions. How to handle this without `_Generic`? (May defer until `_Generic` fully working.)

7. **Implicit conversion to bool:** What should `if (complex_var)` do?
   - Error (safest)
   - True if non-zero (real OR imag != 0)
   - True if real != 0 (discard imag)

   **C99 spec:** Allows conversion to bool (6.3.1.2) - non-zero if either part non-zero.

---

## Change Log

| Date | Decision | Notes |
|------|----------|-------|
| TBD | Type representation | Chose Option A (new VT_BTYPE) |
| TBD | IR representation | Chose Option B (lower to scalar) |
| TBD | Register allocation | Chose Option A (treat as unit) |
