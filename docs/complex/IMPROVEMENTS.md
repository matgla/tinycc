# Complex Number Implementation Plan - Improvements Made

This document summarizes improvements made to the original implementation plan.

## Critical Issues Fixed

### 1. **VT_BTYPE Mask Overflow (BLOCKER)**

**Problem:** Original plan proposed `VT_CDOUBLE = 16`, but `VT_BTYPE` mask is `0x000f` (max value 15).

**Solution:** Added clear decision point with two options:
- **Option A (Recommended):** Expand VT_BTYPE from 0x000f to 0x001f (5 bits)
  - Requires auditing ~50-100 code locations
  - More future-proof (supports up to 31 types)

- **Option B (Fallback):** Use VT_COMPLEX flag bit
  - More complex type checking throughout codebase
  - Fallback if mask expansion too risky

**Files Updated:**
- `README.md` §1.1 - Added critical decision point
- `DESIGN_DECISIONS.md` Decision 1 - Added implementation steps for mask expansion
- `GETTING_STARTED.md` - Added prominent warning before Step 1
- `IMPLEMENTATION_CHECKLIST.md` - Added Phase 0.2 for VT_BTYPE audit

---

## Major Additions

### 2. **Phase 0: Research and Preparation**

**Why Added:** Original plan jumped directly to implementation without validating approach.

**New Phase 0 includes:**
- ABI research (ARM AAPCS §4.1.2)
- Study GCC/Clang implementations
- VT_BTYPE mask audit
- Prototype struct-based approach
- ABI compatibility testing
- **Decision point before committing to implementation strategy**

**Files Updated:**
- `README.md` - Added complete Phase 0 section
- `IMPLEMENTATION_CHECKLIST.md` - Added Phase 0 tasks
- `GETTING_STARTED.md` - Added warning to complete Phase 0 first

### 3. **Type Conversion Rules**

**Problem:** Original plan didn't specify how type conversions work.

**Added:**
- Real ↔ Complex conversions (C99 6.3.1.7)
- Complex ↔ Complex (widening/narrowing)
- Integer → Complex
- Explicit casts
- Complex → Bool (C99 6.3.1.2)

**Files Updated:**
- `README.md` §1.5 - New subsection on type conversion
- `IMPLEMENTATION_CHECKLIST.md` §1.6 - Conversion implementation tasks
- `TEST_PLAN.md` - New "Type Conversion Tests" section

### 4. **ABI Calling Convention Details**

**Problem:** Calling convention was Phase 7 but affects design from start.

**Added:**
- Moved AAPCS details earlier (Phase 3.0)
- Documented exact register usage for soft-float and VFP
- Clarified atomic treatment of complex values
- Stack overflow handling

**Files Updated:**
- `README.md` §3.0 - New subsection before code generation

---

## Test Coverage Improvements

### 5. **Critical ABI Compatibility Tests**

**Added:**
- GCC-compiled function called from TCC
- TCC-compiled function called from GCC
- Stack parameter passing tests

**Files Updated:**
- `TEST_PLAN.md` - New "ABI Compatibility Tests" section (critical)

### 6. **Union and Aliasing Tests**

**Added:**
- Complex in unions
- Pointer aliasing tests
- Layout compatibility tests

**Files Updated:**
- `TEST_PLAN.md` - New "Union and Aliasing Tests" section

### 7. **Type Conversion Tests**

**Added:**
- Real → Complex
- Complex → Real
- Widening/narrowing
- Integer conversions
- Cast operations

**Files Updated:**
- `TEST_PLAN.md` - New "Type Conversion Tests" section

---

## Design Decision Enhancements

### 8. **Expanded Open Questions**

**Added:**
- Question about struct-based vs native implementation
- VT_BTYPE mask expansion risk assessment
- Complex to bool conversion behavior

**Files Updated:**
- `DESIGN_DECISIONS.md` - Expanded from 4 to 7 questions with recommendations

---

## Documentation Structure Improvements

### 9. **Clear Decision Points**

**Before:** Plan assumed one implementation path.

**After:** Multiple decision points with clear criteria:
1. Phase 0: Choose implementation strategy
2. Phase 1: VT_BTYPE mask size decision
3. Phase 3: Inline vs runtime for complex operations

### 10. **Risk Callouts**

Added prominent warnings for:
- VT_BTYPE overflow risk
- ABI compatibility requirements
- Phase 0 prerequisite

---

## Summary of File Changes

| File | Lines Added | Key Improvements |
|------|-------------|------------------|
| `README.md` | ~80 | Phase 0, VT_BTYPE fix, type conversion, AAPCS details |
| `DESIGN_DECISIONS.md` | ~40 | Mask expansion steps, expanded open questions |
| `TEST_PLAN.md` | ~100 | ABI tests, conversion tests, union tests |
| `IMPLEMENTATION_CHECKLIST.md` | ~30 | Phase 0 tasks, conversion tasks |
| `GETTING_STARTED.md` | ~20 | Critical warning, mask expansion step |
| `IMPROVEMENTS.md` | New | This document |

**Total:** ~270 lines added/modified

---

## Remaining Risks

### High Priority
1. **VT_BTYPE mask expansion** - Could break existing code if flags conflict
2. **ABI compatibility** - Must match GCC exactly or interop fails
3. **Register allocator** - Handling register pairs may be complex

### Medium Priority
4. **Complex division** - Mathematically complex, many edge cases
5. **Debug info** - DWARF generation may need updates
6. **Performance** - Inline vs runtime tradeoffs

### Low Priority
7. **Type-generic math** - Deferred to post-MVP
8. **Complex integers** - GCC extension, low priority

---

## Recommended Next Steps

1. **Complete Phase 0** (estimated 1-2 days)
   - Read ARM AAPCS carefully
   - Count VT_BTYPE uses: `grep -rn "VT_BTYPE" *.c *.h | wc -l`
   - Prototype struct-based approach
   - Make implementation decision

2. **If choosing mask expansion:**
   - Create feature branch
   - Expand VT_BTYPE to 0x001f
   - Run full test suite
   - Fix regressions before proceeding

3. **If choosing struct-based:**
   - Define internal complex struct type
   - Map _Complex to struct in parser
   - Implement __real__/__imag__ as special accessors

4. **Implement incrementally:**
   - Start with Phase 1 (types only)
   - Test thoroughly before Phase 2
   - Get each phase working before next

5. **Test ABI compatibility early:**
   - Don't wait until Phase 7
   - Test calling convention after basic codegen works

---

## Questions for Reviewer

1. **VT_BTYPE expansion:** Is expanding the mask acceptable? Any known conflicts?
2. **Struct-based approach:** Should we seriously consider this as primary path?
3. **Implementation effort:** With improvements, estimate now ~3-4 weeks vs original 2-3 weeks. Acceptable?
4. **Test coverage:** Are ABI compatibility tests sufficient?
5. **Deferred features:** Agree on deferring complex integers and _Generic to post-MVP?

---

## Conclusion

The improved plan is more robust with:
- ✅ Critical VT_BTYPE issue addressed
- ✅ Phase 0 research prevents costly rework
- ✅ Type conversion rules specified
- ✅ ABI compatibility prioritized
- ✅ Test coverage expanded
- ✅ Clear decision points identified

**Status:** Plan ready for Phase 0 implementation.
