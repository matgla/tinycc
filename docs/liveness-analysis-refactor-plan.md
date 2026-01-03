# Liveness Analysis Refactoring Plan

## Goal
Move liveness analysis from incremental tracking during `tcc_ir_put()` to a single-pass computation after all optimizations are complete.

## Current Problems

1. **Stale data after optimizations** - Intervals computed during IR construction become invalid when:
   - Dead code elimination removes instructions
   - Copy propagation changes operand vregs
   - CSE replaces uses with different vregs
   - Constant propagation eliminates operations

2. **Redundant work** - `tcc_ir_find_live_interval()` already re-scans the IR to handle backward jumps

3. **Complex invariants** - Optimizations must be careful not to break interval tracking

## Current Architecture

### During `tcc_ir_put()` (tccir.c:771)
```c
// For src1/src2:
tcc_ir_set_base_interval_end(ir, src1->vr);  // Updates interval->end

// For dest:
if (dest_interval->start == INTERVAL_NOT_STARTED) {
    dest_interval->start = ir->next_instruction_index;
}
dest_interval->end = ir->next_instruction_index;
```

### During `tcc_ir_liveness_analysis()` (tccir.c:1259)
- Calls `tcc_ir_find_live_interval()` which re-scans for backward jumps
- Calls `tcc_ir_extend_param_intervals()` for function params
- Checks `tcc_ir_has_call_in_range()` for crosses_call

## Proposed Architecture

### Phase 1: Remove Tracking from `tcc_ir_put()`

**File: tccir.c**

1. Remove calls to `tcc_ir_set_base_interval_end()` in `tcc_ir_put()`
2. Remove setting of `interval->start` and `interval->end` in `tcc_ir_put()`
3. Keep: Type tracking (`tcc_ir_set_float_type`, `tcc_ir_set_llong_type`)
4. Keep: `is_lvalue` tracking (needed for code generation)
5. Keep: `addrtaken` tracking (set elsewhere when address is taken)

### Phase 2: Enhance `tcc_ir_liveness_analysis()`

Create new function `tcc_ir_compute_live_intervals()` that:

```c
void tcc_ir_compute_live_intervals(TCCIRState *ir)
{
    // Reset all intervals
    for each vreg:
        interval->start = INTERVAL_NOT_STARTED;
        interval->end = 0;

    // Single forward pass over IR
    for (int i = 0; i < ir->next_instruction_index; i++) {
        TACQuadruple *q = &ir->instructions[i];
        if (q->op == TCCIR_OP_NOP) continue;

        // Process dest (definition)
        if (has_dest && vreg_valid(q->dest.vr)) {
            interval = get_interval(q->dest.vr);
            if (interval->start == INTERVAL_NOT_STARTED) {
                interval->start = i;
            }
            interval->end = i;  // Def counts as a use point
        }

        // Process src1 (use)
        if (has_src1 && vreg_valid(q->src1.vr)) {
            interval = get_interval(q->src1.vr);
            if (interval->start == INTERVAL_NOT_STARTED) {
                // Use before def - parameter or error
                interval->start = 0;
            }
            interval->end = i;
        }

        // Process src2 (use)
        if (has_src2 && vreg_valid(q->src2.vr)) {
            interval = get_interval(q->src2.vr);
            if (interval->start == INTERVAL_NOT_STARTED) {
                interval->start = 0;
            }
            interval->end = i;
        }
    }

    // Handle backward jumps (extend intervals for loop variables)
    tcc_ir_extend_for_backward_jumps(ir);

    // Extend intervals for function parameters
    tcc_ir_extend_param_intervals(ir);
}
```

### Phase 3: Update `tcc_ir_liveness_analysis()`

Simplify to:
```c
void tcc_ir_liveness_analysis(TCCIRState *ir)
{
    tcc_ls_clear_live_intervals(&ir->ls);

    // Compute fresh intervals from IR
    tcc_ir_compute_live_intervals(ir);

    // Copy to linear scan allocator
    for each vreg type (VAR, TEMP, PARAM):
        for each vreg:
            if (interval->start != INTERVAL_NOT_STARTED) {
                crosses_call = tcc_ir_has_call_in_range(ir, start, end);
                tcc_ls_add_live_interval(&ir->ls, ...);
            }
}
```

## Files to Modify

| File | Changes |
|------|---------|
| tccir.c | Remove interval tracking from `tcc_ir_put()`, add `tcc_ir_compute_live_intervals()` |
| tccir.h | Add declaration for `tcc_ir_compute_live_intervals()` if public |

## Detailed Changes

### 1. tccir.c: `tcc_ir_put()` (~line 815-875)

**Remove:**
```c
// Lines ~815-817
if (tcc_is_vreg_valid(ir, src1->vr)) {
    tcc_ir_set_base_interval_end(ir, src1->vr);
}

// Lines ~833-835
if (tcc_is_vreg_valid(ir, src2->vr)) {
    tcc_ir_set_base_interval_end(ir, src2->vr);
}

// Lines ~867-878
if (dest_interval->start == INTERVAL_NOT_STARTED) {
    dest_interval->start = ir->next_instruction_index;
    if (ir->processing_if && TCCIR_DECODE_VREG_TYPE(dest->vr) == TCCIR_VREG_TYPE_VAR) {
        dest_interval->start_within_if = 1;
    }
}
dest_interval->end = ir->next_instruction_index;
```

**Keep:**
```c
// Type tracking (needed for register allocation)
if (tcc_ir_is_float_type(dest->type.t)) {
    tcc_ir_set_float_type(ir, dest->vr, 1, tcc_ir_is_double_type(dest->type.t));
} else if ((dest->type.t & VT_BTYPE) == VT_LLONG) {
    tcc_ir_set_llong_type(ir, dest->vr);
}

// is_lvalue tracking
dest_interval->is_lvalue = (op != TCCIR_OP_LOAD);
```

### 2. tccir.c: New `tcc_ir_compute_live_intervals()`

Add before `tcc_ir_liveness_analysis()`:
- Iterate all instructions once
- Track first def (start) and last use (end) for each vreg
- Handle NOP instructions (skip them)

### 3. tccir.c: Simplify `tcc_ir_find_live_interval()`

This function can be simplified or removed since intervals are computed fresh.
Only keep backward jump extension logic.

### 4. tccir.c: `tcc_ir_liveness_analysis()`

- Call `tcc_ir_compute_live_intervals()` first
- Remove redundant re-computation

## Testing Strategy

1. Run existing IR tests: `tests/ir_tests/`
2. Verify register allocation produces same results
3. Check optimized code still works correctly
4. Test with loops (backward jumps)
5. Test with function calls (crosses_call)

## Benefits

1. **Correctness** - Intervals always match actual IR after optimizations
2. **Simplicity** - Single source of truth for liveness
3. **Performance** - One pass instead of incremental + re-scan
4. **Maintainability** - Optimizations don't need to maintain intervals

## Risks

1. **is_lvalue tracking** - May need to compute during IR scan instead of put
2. **addrtaken tracking** - Need to ensure this is still captured
3. **Type tracking** - Must preserve float/llong type info

## Migration Path

1. Implement new `tcc_ir_compute_live_intervals()`
2. Call it before existing `tcc_ir_liveness_analysis()`
3. Verify results match
4. Remove old tracking from `tcc_ir_put()`
5. Clean up unused functions
