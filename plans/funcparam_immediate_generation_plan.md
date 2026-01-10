# Function Parameter Immediate Generation Plan

## Goal
Simplify function call argument handling by:
1. **Immediate generation**: Process arguments when `IR_FUNCPARAM` is generated, no caching on vtop
2. **Call ID tracking**: Add function call ID to parameter instructions to handle nested calls
3. **Unified processing**: Single loop in tccgen.c with reverse argument order
4. **ABI-driven placement**: Use tccabi.h API to decide register vs stack placement

## Current Problems

### 1. Cache-based approach complexity
- Arguments are evaluated and cached on vtop stack
- First 4 args processed in one loop ([tccgen.c:7315-7320](../tccgen.c))
- Remaining args processed in separate loop ([tccgen.c:7357-7366](../tccgen.c))
- Complex vtop management with indices like `vtop[-2]`, making code hard to follow

### 2. Split register/stack processing
- Two separate loops for register args vs stack args
- Frontend (tccgen.c) must know about ABI details (4 register args on ARM)
- Hard-coded magic number `nb_args < 4` in multiple places
- Violates separation of concerns (frontend shouldn't know ABI)

### 3. Nested call detection is implicit
- Nested calls detected via complex backward scanning in IR
- `tcc_ir_build_callsites()` must track `nested_call_depth` ([funcparam_refactor_plan.md:33](funcparam_refactor_plan.md))
- No explicit marker of which FUNCPARAMVAL belongs to which call
- Fragile in presence of optimizations

### 4. Reverse argument processing
- Current code has special handling for `reverse_funcargs` ([tccgen.c:7333-7349](../tccgen.c))
- Requires saving argument expressions and re-parsing them
- Adds significant complexity to support right-to-left evaluation

## Proposed Architecture

### Overview
```
tccgen.c (frontend)          tccir.c (IR layer)          backend (arm-thumb-gen.c)
─────────────────────────────────────────────────────────────────────────────

Parse arguments              Build callsites:            Materialize call:
in REVERSE order    ───────> - Group by call_id  ─────>  - Fetch IRCallSite
                             - Query ABI API             - Place args per layout
Emit FUNCPARAMVAL            - Compute layout            - Emit machine code
with call_id
                             Store IRCallSite with
No vtop caching              TCCAbiCallLayout
Single unified loop
```

### Key Changes

#### 1. Add call_id to distinguish arguments
**In tccgen.c:**
```c
// Each function call gets a unique ID
int current_call_id = ir->next_call_id++;

// When emitting FUNCPARAMVAL, encode the call_id
SValue call_marker;
call_marker.c.i = current_call_id;  // src2 carries call_id
call_marker.c.i64 = param_index;     // or encode both in single int64

tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, vtop, &call_marker, NULL);
```

**Rationale:**
- Explicit binding of parameters to calls
- Trivial nested call handling: inner call has different call_id
- No backward scanning needed to determine ownership

#### 2. Process arguments immediately (no cache)
**Current approach:**
```c
// Parse all arguments first
for (i = 0; i < nb_args; i++) {
    expr_eq();
    gfunc_param_typed(s, sa);
    // argument stays on vtop
}
// Then process them later in 2 loops (register vs stack)
```

**Proposed approach:**
```c
// Process each argument immediately when parsed
int call_id = ir->next_call_id++;
int param_index = 0;

// Parse in reverse order for right-to-left evaluation
while (parse_next_argument()) {
    expr_eq();
    gfunc_param_typed(s, sa);

    // Emit immediately - no caching
    SValue call_info;
    call_info.c.i = (call_id << 16) | param_index;
    tcc_ir_put(ir, TCCIR_OP_FUNCPARAMVAL, vtop, &call_info, NULL);

    vtop--;  // consumed
    param_index++;
}

// Emit call with same call_id
SValue call_marker;
call_marker.c.i = call_id;
tcc_ir_put(ir, TCCIR_OP_FUNCCALLVAL, target, &call_marker, &dest);
```

**Benefits:**
- No vtop index arithmetic (`vtop[-2]`, etc.)
- Each argument processed exactly once
- Simpler control flow

#### 3. Single unified loop - no register/stack split
**Remove from tccgen.c:**
- `nb_args < 4` checks
- Separate loops for register args vs stack args
- Hard-coded knowledge of ARM calling convention

**Single loop emits all arguments:**
```c
for (each argument in reverse order) {
    // Parse and type-check
    expr_eq();
    gfunc_param_typed(s, sa);

    // Emit to IR immediately
    emit_funcparam(call_id, param_index, vtop);

    vtop--;
}
```

**Where does each arg go? Let tccir.c decide via ABI API.**

#### 4. ABI API decides placement
**In tccir.c - when building callsites:**
```c
void tcc_ir_build_callsites(TCCIRState *ir) {
    // Scan for FUNCCALL* instructions
    // Group FUNCPARAMVAL by call_id (from src2.c.i)

    for (each callsite) {
        // Build argument descriptors
        TCCAbiArgDesc *arg_descs = tcc_malloc(sizeof(*arg_descs) * argc);
        for (int i = 0; i < argc; i++) {
            arg_descs[i] = describe_argument(&callsite->args[i]);
        }

        // Query target ABI: where does each argument go?
        TCCAbiCallLayout layout = {0};
        layout.argc = argc;
        layout.locs = tcc_malloc(sizeof(TCCAbiArgLoc) * argc);

        tcc_target_compute_call_layout(arg_descs, argc, &layout);

        // Now we know:
        // - arg[0] -> R0 (layout.locs[0].kind = TCC_ABI_LOC_REG, reg_base=0)
        // - arg[1] -> R1
        // - arg[5] -> stack offset 0 (layout.locs[5].kind = TCC_ABI_LOC_STACK)

        // Store layout for backend
        callsite->abi_layout = layout;
    }
}
```

**The ABI API ([tccabi.h](../tccabi.h)) already defines:**
- `TCCAbiArgDesc` - describe argument (scalar32, scalar64, struct)
- `TCCAbiArgLoc` - placement decision (register or stack + details)
- `TCCAbiCallLayout` - complete layout for all arguments

**Backend implementation** (per-target):
```c
// In arm-thumb-gen.c or target-specific file
void tcc_target_compute_call_layout(
    const TCCAbiArgDesc *args,
    int argc,
    TCCAbiCallLayout *layout)
{
    int next_reg = 0;  // R0-R3 available
    int stack_offset = 0;

    for (int i = 0; i < argc; i++) {
        if (args[i].kind == TCC_ABI_ARG_SCALAR32) {
            if (next_reg < 4) {
                // Fits in register
                layout->locs[i].kind = TCC_ABI_LOC_REG;
                layout->locs[i].reg_base = next_reg++;
                layout->locs[i].reg_count = 1;
            } else {
                // Spill to stack
                layout->locs[i].kind = TCC_ABI_LOC_STACK;
                layout->locs[i].stack_off = stack_offset;
                stack_offset += 4;
            }
        }
        else if (args[i].kind == TCC_ABI_ARG_SCALAR64) {
            // long long: needs 2 registers, aligned
            next_reg = (next_reg + 1) & ~1;  // align to even
            if (next_reg + 1 < 4) {
                layout->locs[i].kind = TCC_ABI_LOC_REG;
                layout->locs[i].reg_base = next_reg;
                layout->locs[i].reg_count = 2;
                next_reg += 2;
            } else {
                // Spill to stack
                stack_offset = (stack_offset + 7) & ~7;  // align 8
                layout->locs[i].kind = TCC_ABI_LOC_STACK;
                layout->locs[i].stack_off = stack_offset;
                stack_offset += 8;
            }
        }
        // ... handle structs, floats, etc.
    }

    layout->stack_size = (stack_offset + 7) & ~7;  // align to 8
    layout->stack_align = 8;
}
```

## Implementation Phases

### Phase 1: Add call_id to FUNCPARAMVAL (minimal change)
**Goal:** Explicit call ownership without changing processing logic

1. Add `next_call_id` counter to `TCCIRState`
2. Allocate unique call_id in tccgen.c when starting a call
3. Encode call_id in `FUNCPARAMVAL` src2 field
4. Update `tcc_ir_build_callsites()` to group by call_id instead of backward scan
5. Verify existing tests still pass

**Changes:**
- [tccir.h](../tccir.h): add `int next_call_id` to `TCCIRState`
- [tccgen.c](../tccgen.c): allocate and pass call_id when emitting FUNCPARAMVAL
- [tccir.c](../tccir.c): group parameters by call_id in `tcc_ir_build_callsites()`

**Acceptance:**
- All existing tests pass
- Nested calls `f(g(1), h(2))` correctly bind arguments
- No change in generated code quality

### Phase 2: Unify argument processing loop
**Goal:** Single loop in tccgen.c, remove register/stack split

1. Remove `nb_args < 4` conditional in tccgen.c
2. Process all arguments in single loop
3. Emit FUNCPARAMVAL immediately for each argument
4. Remove vtop cache management

**Changes:**
- [tccgen.c:7300-7366](../tccgen.c): replace two loops with single unified loop
- Remove special handling for "first 4 args"

**Acceptance:**
- All tests pass
- Code is simpler and more readable
- Frontend no longer has hard-coded ABI knowledge

### Phase 3: Implement ABI query API
**Goal:** Let target backend decide argument placement

1. Implement `tcc_target_compute_call_layout()` for ARM
2. Call it from `tcc_ir_build_callsites()`
3. Store `TCCAbiCallLayout` in `IRCallSite`
4. Update backend to consume layout instead of computing placement

**Changes:**
- New function in [arm-thumb-gen.c](../arm-thumb-gen.c): `tcc_target_compute_call_layout()`
- [tccir.c](../tccir.c): call ABI query and store layout
- [tccir.h](../tccir.h): add `TCCAbiCallLayout *abi_layout` to `IRCallSite`
- Backend call lowering: use pre-computed layout

**Acceptance:**
- All tests pass
- Generated code matches previous quality
- Adding new targets only requires implementing ABI function

### Phase 4: Support reverse argument evaluation
**Goal:** Proper right-to-left evaluation without special reverse_funcargs

Current code has complex handling for `reverse_funcargs` configuration.
With immediate generation, we can parse arguments in reverse order naturally.

**Options:**
1. Always use reverse order (right-to-left per C standard)
2. Make it configurable if needed for compatibility
3. Parse forward but emit in reverse order

**Recommended:** Parse arguments forward (easier for parsing), but assign parameter indices in reverse.

```c
// Parse: arg0, arg1, arg2
// But emit with param_index: 2, 1, 0
// So evaluation order is left-to-right, but call semantics are reversed
```

This satisfies both parsing simplicity and C semantics.

## Nested Call Handling

### Current approach (implicit)
```c
f(g(1), h(2), 3)

IR stream:
  FUNCPARAMVAL vreg=1, param_num=0   // g's argument
  FUNCCALL vreg=10                   // call g(1) -> vreg=10
  FUNCPARAMVAL vreg=10, param_num=0  // f's first argument
  FUNCPARAMVAL vreg=2, param_num=0   // h's argument
  FUNCCALL vreg=20                   // call h(2) -> vreg=20
  FUNCPARAMVAL vreg=20, param_num=1  // f's second argument
  FUNCPARAMVAL vreg=3, param_num=2   // f's third argument
  FUNCCALL                           // call f
```

Problem: Three `param_num=0` instructions - which belongs to which call?
Current solution: Backward scan with nested call depth tracking (fragile).

### Proposed approach (explicit call_id)
```c
f(g(1), h(2), 3)

IR stream:
  FUNCPARAMVAL vreg=1, call_id=100, param_num=0   // g(1)
  FUNCCALL vreg=10, call_id=100

  FUNCPARAMVAL vreg=10, call_id=101, param_num=0  // f's arg0 = g()

  FUNCPARAMVAL vreg=2, call_id=102, param_num=0   // h(2)
  FUNCCALL vreg=20, call_id=102

  FUNCPARAMVAL vreg=20, call_id=101, param_num=1  // f's arg1 = h()
  FUNCPARAMVAL vreg=3, call_id=101, param_num=2   // f's arg2 = 3

  FUNCCALL call_id=101                            // f()
```

Grouping algorithm:
```c
for (each FUNCCALL at index i) {
    int call_id = extract_call_id(instructions[i]);

    // Collect all FUNCPARAMVAL with matching call_id
    for (int j = i-1; j >= 0; j--) {
        if (instructions[j].op == FUNCPARAMVAL) {
            int param_call_id = extract_call_id(instructions[j]);
            if (param_call_id == call_id) {
                add_to_callsite(j);
            }
        }
        // No need to track nesting depth!
    }
}
```

**Benefits:**
- Trivial to implement
- Robust against IR reordering
- No nested call depth tracking needed
- Works with any level of nesting

## Register Pressure & Spilling

### Concern: Immediate generation increases live ranges
If arguments are generated immediately, their vregs are live longer:

```c
// Before (cached):
f(expensive1(), expensive2(), expensive3())
// All three expressions evaluated
// Then processed together - shorter live ranges

// After (immediate):
f(expensive1(), expensive2(), expensive3())
// expensive1() result live while expensive2() and expensive3() run
```

### Why this is actually better:
1. **More accurate liveness** - this is the true lifetime
2. **Register allocator can handle it** - spill if needed
3. **Matches C semantics** - arguments must be live until call
4. **Current code has same issue** - just hidden by vtop

The current vtop approach doesn't actually reduce register pressure - it just defers the problem. The argument values must be preserved until the call regardless of how they're cached.

## Backward Compatibility

### Existing IR format
Current `FUNCPARAMVAL` has:
- `src1`: argument value (SValue)
- `src2.c.i`: parameter number (0-based)

### Proposed encoding
**Option A: Use full int64 in src2**
```c
src2.c.i64 = ((uint64_t)call_id << 32) | param_num;
```

**Option B: Add aux field**
```c
src2.c.i = param_num;   // keep for backward compat
quadruple->aux = call_id;
```

**Option C: Separate instruction**
```c
TCCIR_OP_CALLSEQ_BEGIN (call_id)
TCCIR_OP_FUNCPARAMVAL (param_num)
...
TCCIR_OP_FUNCPARAMVAL (param_num)
TCCIR_OP_FUNCCALL
TCCIR_OP_CALLSEQ_END
```

**Recommendation: Option A (int64 encoding)**
- Simple
- No extra instructions
- Backward compatible (old code only reads low 32 bits)
- Easy to extract: `call_id = src2.c.i64 >> 32; param_num = src2.c.i64 & 0xFFFFFFFF;`

## Testing Requirements

### Unit tests (add to tests/ir_tests/)
1. **Simple call**: `f(1, 2, 3)` - verify basic case
2. **Nested calls**: `f(g(1), h(2))` - verify call_id disambiguation
3. **Deep nesting**: `f(g(h(i(1))))` - stress test
4. **Many args**: `f(a,b,c,d,e,f,g,h,i,j)` - >4 args, mixed register/stack
5. **64-bit args**: `f(1LL, 2LL, 3LL, 4LL)` - register alignment
6. **Struct args**: `f(struct1, struct2)` - various sizes
7. **Mixed args**: `f(1, 2LL, struct, 3)` - complex layout
8. **Zero args**: `f()` - edge case

### Integration tests
- All existing TCC tests must pass
- Bootstrap test (compile TCC with TCC)
- Real-world code compilation

### Performance tests
- Verify no regression in compilation speed
- Check generated code quality (should be same or better)

## Migration Path

### Step 1: Implement behind flag
```c
#ifdef FUNCPARAM_IMMEDIATE_GENERATION
  // New code path
#else
  // Old code path (current)
#endif
```

### Step 2: Validate both paths produce identical code
Run all tests with both paths, compare generated assembly.

### Step 3: Enable by default, deprecate old path
Once validated, make new path default.

### Step 4: Remove old code
After grace period (1 release), remove old implementation.

## Benefits Summary

### Code Quality
- **Simpler tccgen.c**: single loop instead of two
- **Clearer intent**: immediate generation is straightforward
- **Less state**: no vtop cache management
- **Fewer bugs**: explicit call_id prevents mis-binding

### Architecture
- **Separation of concerns**: frontend doesn't know ABI
- **Extensibility**: new targets only implement ABI function
- **Robustness**: explicit ownership survives optimizations
- **Maintainability**: less special-case code

### Performance
- **Compilation speed**: fewer IR passes (no backward scanning)
- **Generated code**: same or better (more accurate liveness)
- **Memory**: less vtop pressure

## Open Questions

1. **Encoding format**: int64 vs aux field vs separate instruction?
   - **Recommendation**: int64 encoding (Option A)

2. **Argument evaluation order**: Always reverse or configurable?
   - **Recommendation**: Always right-to-left (C standard)

3. **Migration timeline**: Phased rollout or big switch?
   - **Recommendation**: Phased (4 phases above)

4. **Float/VFP handling**: Does this change affect VFP register allocation?
   - **Analysis needed**: Check if float args need special handling

5. **Soft-float library calls**: How do helper calls interact?
   - **Analysis needed**: Review `tcc_ir_put_soft_call()` paths

## References

- [funcparam_refactor_plan.md](funcparam_refactor_plan.md) - Original refactoring plan (Phases 1-2 complete)
- [tccabi.h](../tccabi.h) - ABI interface definitions (already exists!)
- [tccgen.c:7300-7366](../tccgen.c) - Current argument processing code
- [tccir.c:672](../tccir.c) - `tcc_ir_build_callsites()` implementation
- [tccir.h:175-182](../tccir.h) - `IRCallSite` structure definition

## Next Steps

1. Review and approve this plan
2. Implement Phase 1 (add call_id)
3. Validate with existing tests
4. Proceed with subsequent phases
5. Update [funcparam_refactor_plan.md](funcparam_refactor_plan.md) status

---

**Author**: Claude (AI Assistant)
**Date**: 2026-01-10
**Status**: PROPOSAL - awaiting review
