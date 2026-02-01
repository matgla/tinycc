# Quick Win 1: Cache Frame Pointer Offset - Implementation Summary

## Status: ✅ IMPLEMENTED

This optimization has been successfully implemented and all tests pass.

---

## Overview

This optimization eliminates redundant frame pointer offset calculations by caching the results of `fp + offset` or `sp + offset` computations. This is a lightweight version of Common Subexpression Elimination (CSE) specifically targeting the most frequent redundant operation in TCC-generated code.

---

## Problem Statement

### Baseline Measurements (Before Optimization)

Measured using `tests/ir_tests/measure_fp_cache.py`:

```
Total FP offset calculations: 12
Breakdown by offset:
    Offset    Count Status
----------------------------------------
#      32:        5  REDUNDANT (4 extra)
#     256:        7  REDUNDANT (6 extra)

Total redundant calculations: 10
Potential savings: 10 instructions (83.3%)

By function:
  test_loop_access   :  7 calcs, 1 unique, 6 redundant
  test_swap_pattern  :  5 calcs, 1 unique, 4 redundant
```

**Key Finding:** 83.3% of frame pointer offset calculations are redundant.

---

## Implementation

### Files Modified

| File | Changes |
|------|---------|
| `tcc.h` | Added `opt_fp_offset_cache` flag to `TCCState` |
| `libtcc.c` | Added flag parsing and enable with `-O1` |
| `arm-thumb-defs.h` | Added `FPOffsetCache` structures and function declarations |
| `arm-thumb-gen.c` | Implemented cache functions and integration |

### Data Structures (`arm-thumb-defs.h`)

```c
#define FP_OFFSET_CACHE_SIZE 8

typedef struct FPOffsetCacheEntry
{
  int8_t valid;         /* Whether this entry is valid */
  int8_t reg;           /* Register holding base_reg + offset */
  int8_t base_reg;      /* Base register: R_FP (r7) or R_SP (r13) */
  int16_t offset;       /* Offset from base (can be negative) */
  uint32_t last_used;   /* LRU counter for eviction */
} FPOffsetCacheEntry;

typedef struct FPOffsetCache
{
  FPOffsetCacheEntry entries[FP_OFFSET_CACHE_SIZE];
  uint32_t access_count;
} FPOffsetCache;
```

### Cache Management Functions (`arm-thumb-gen.c`)

```c
ST_FUNC void tcc_fp_cache_init(FPOffsetCache *cache);
ST_FUNC int tcc_fp_cache_lookup(FPOffsetCache *cache, int base_reg, int offset);
ST_FUNC void tcc_fp_cache_record(FPOffsetCache *cache, int reg, int base_reg, int offset);
ST_FUNC void tcc_fp_cache_invalidate_reg(FPOffsetCache *cache, int reg);
ST_FUNC void tcc_fp_cache_clear(FPOffsetCache *cache);
```

### Integration in `tcc_machine_addr_of_stack_slot()`

The optimization is **conservative** - only caches for `ip` (R12) register:

```c
/* Only use cache for IP register which is caller-saved and less likely to be 
 * reused for other purposes within the same expression. */
if (tcc_state->opt_fp_offset_cache && dest_reg == R_IP) {
  FPOffsetCache *cache = &thumb_gen_state.fp_offset_cache;
  int cached_reg = tcc_fp_cache_lookup(cache, base_reg, frame_offset);
  if (cached_reg != PREG_REG_NONE) {
    /* Cache hit! */
    if (dest_reg != cached_reg) {
      ot_check(th_mov_reg(dest_reg, cached_reg, ...));
    }
    return;
  }
}

/* ... compute address ... */

/* Record in cache for future reuse (only when optimization enabled and dest is IP) */
if (tcc_state->opt_fp_offset_cache && dest_reg == R_IP) {
  tcc_fp_cache_record(cache, dest_reg, base_reg, frame_offset);
}
```

### Cache Invalidation Points

1. **Function entry**: `tcc_fp_cache_init()` called at prologue
2. **Function calls**: `tcc_fp_cache_clear()` in `gcall_or_jump_ir()`
3. **Scratch register allocation**: `tcc_fp_cache_invalidate_reg()` in `get_scratch_reg_with_save()`

### Flag Enablement

```bash
# Enable with -O1 (default)
./armv8m-tcc -O1 -c test.c

# Enable explicitly
./armv8m-tcc -ffp-offset-cache -c test.c

# Disable explicitly (even with -O1)
./armv8m-tcc -O1 -fno-fp-offset-cache -c test.c
```

---

## Test Results

### Unit Test: `test_fp_offset_cache.c`

```bash
$ python -m pytest test_qemu.py -k "test_fp_offset_cache" -v
test_qemu.py::test_qemu_execution[test_fp_offset_cache-O0] PASSED
test_qemu.py::test_qemu_execution[test_fp_offset_cache-O1] PASSED
```

### Full Test Suite

```bash
$ python -m pytest test_qemu.py -v --tb=no -q
============================= 480 passed in 28.28s =============================
```

**All 480 tests pass** - no regressions introduced.

---

## Current Limitations

### Conservative Implementation

The current implementation only caches for the `ip` (R12) register to ensure correctness:

```c
if (tcc_state->opt_fp_offset_cache && dest_reg == R_IP) { ... }
```

**Reason**: The `ip` register is frequently used as a temporary register (e.g., `ldr.w ip, [r3]`), which invalidates the cache. Other registers (r0-r11) may hold live values that get overwritten, making cache invalidation complex.

### Limited Impact

Due to the conservative approach and `ip` register pressure, the optimization's impact is currently limited. The cache is frequently invalidated because:

1. `ip` is used for temporary values in many operations
2. Function calls clear the entire cache
3. Scratch register allocation invalidates cached registers

---

## Future Improvements

1. **Use callee-saved register (r4-r11)** for better cache retention
   - Would require saving/restoring the register
   - Less frequent invalidation

2. **Smarter invalidation**
   - Track which registers are actually modified
   - Only invalidate when necessary

3. **Cross-basic-block caching**
   - Currently cleared at function calls
   - Could retain cache for leaf functions

4. **Extend to other address calculations**
   - Global variable addresses (PC-relative)
   - Struct field offsets

---

## Implementation Checklist

- [x] Create test `test_fp_offset_cache.c` with expected output
- [x] Verify baseline measurements showing redundancy
- [x] Add `FPOffsetCache` structures to `arm-thumb-defs.h`
- [x] Add `fp_offset_cache` to `ThumbGeneratorState`
- [x] Add `opt_fp_offset_cache` flag to `TCCState` in `tcc.h`
- [x] Add flag parsing entry in `libtcc.c` `options_f` array
- [x] Enable flag with `-O1` in `libtcc.c`
- [x] Implement `tcc_fp_cache_init()`
- [x] Implement `tcc_fp_cache_lookup()`
- [x] Implement `tcc_fp_cache_record()`
- [x] Implement `tcc_fp_cache_invalidate_reg()`
- [x] Implement `tcc_fp_cache_clear()`
- [x] Modify `tcc_machine_addr_of_stack_slot()` to check flag and use cache
- [x] Add cache initialization at function entry (conditional on flag)
- [x] Add cache invalidation at register allocation
- [x] Add cache invalidation at function calls
- [x] Run test to verify optimization works
- [x] Verify no regressions in test suite (480/480 pass)

---

## References

- Original analysis: `TCC_OPTIMIZATION_PLAN.md` Phase 1 and Quick Win #1
- Test file: `tests/ir_tests/test_fp_offset_cache.c`
- Analysis script: `tests/ir_tests/measure_fp_cache.py`
- Existing spill cache: `tcc_ir_spill_cache_*` functions in `arm-thumb-gen.c`
