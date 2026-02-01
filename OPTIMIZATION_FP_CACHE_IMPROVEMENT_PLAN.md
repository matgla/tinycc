# FP Offset Cache Improvement Plan: Register-Agnostic Caching

## Status: ✅ IMPLEMENTED (Limited Effectiveness)

The register-agnostic caching has been implemented with a conservative approach (callee-saved registers only). All 480 tests pass, but **actual code size savings are minimal** due to register allocator behavior.

### Root Cause of Limited Impact

The register allocator prefers **caller-saved registers** (r0-r3, ip) for short-lived address calculations, but these registers are too frequently clobbered to cache safely. The cache now only operates on **callee-saved registers** (r4-r11), which the allocator rarely uses for address computations.

**This is a fundamental tension**: Safe caching requires stable registers, but the allocator uses volatile registers for efficiency.

---

## Problem Statement

The original FP offset cache optimization only triggered when `dest_reg == R_IP`, but the register allocator assigns different destination registers for address calculations. The goal was to enable register-agnostic caching where any register holding a computed offset could be reused.

### Original Flow (Before Improvement)

```
Instruction 1: want r3 = fp - 256
  → dest_reg (r3) != R_IP, skip cache lookup
  → compute sub.w r3, r7, #256
  → skip cache record (dest != R_IP)

Instruction 2: want ip = fp - 256
  → dest_reg (ip) == R_IP, check cache
  → MISS (nothing was recorded!)
  → compute sub.w ip, r7, #256
```

**Result**: 0 cache hits

---

## Solution: Register-Agnostic Cache

### Core Idea

The cache should answer: *"Do we already have `base_reg + offset` computed in ANY register?"*

If yes, emit a MOV instead of recomputing:
```asm
194: f5a7 7380  sub.w   r3, r7, #256    # First computation
1ce: 4663       mov     ip, r3          # Reuse via MOV (2 bytes vs 4!)
```

### Proposed Flow

```
Instruction 1: want r3 = fp - 256
  → lookup cache for (fp, -256)
  → MISS
  → compute sub.w r3, r7, #256
  → record: cache[fp-256] = r3  ← ALWAYS RECORD

Instruction 2: want ip = fp - 256
  → lookup cache for (fp, -256)
  → HIT! cached_reg = r3
  → emit: mov ip, r3  (instead of sub.w ip, r7, #256)
  → optionally update cache: cache[fp-256] = ip (LRU refresh)
```

**Result**: 1 cache hit, 1 MOV saved (and possibly smaller encoding)

---

## Implementation

### What Was Implemented

**Conservative approach: Cache only callee-saved registers (r4-r11)**

After attempting full register-agnostic caching, correctness issues arose because caller-saved registers (r0-r3, r12) are too frequently clobbered. The implemented solution restricts caching to callee-saved registers which are preserved across function calls and less volatile.

**File**: `arm-thumb-gen.c` in `tcc_machine_addr_of_stack_slot()`

```c
/* Check cache for callee-saved registers only (r4-r11) */
if (tcc_state->opt_fp_offset_cache && dest_reg >= 4 && dest_reg <= 11) {
  int cached_reg = tcc_fp_cache_lookup(cache, base_reg, frame_offset);
  if (cached_reg != PREG_REG_NONE && cached_reg >= 4 && cached_reg <= 11
      && cached_reg != dest_reg) {
    /* Safe to reuse via MOV */
    ot_check(th_mov_reg(dest_reg, cached_reg, ...));
    return;
  }
}

/* Record only for callee-saved registers */
if (tcc_state->opt_fp_offset_cache && dest_reg >= 4 && dest_reg <= 11) {
  tcc_fp_cache_record(cache, dest_reg, base_reg, frame_offset);
}
```

### Invalidation Strategy

Register invalidation happens at these points:

| Event | Implementation | File |
|-------|---------------|------|
| Function entry | `tcc_fp_cache_init()` | `gen_function_prologue()` |
| Function call | `tcc_fp_cache_clear()` | `gcall_or_jump_ir()` |
| Scratch reg allocation | `tcc_fp_cache_invalidate_reg()` | `get_scratch_reg_with_save()` |
| Register write | `tcc_fp_cache_invalidate_reg()` | `load_to_dest_ir()` |

---

## Test Results

### Correctness

✅ **All 480 tests pass**
- `test_qemu.py` complete suite: 480/480 passed
- No regressions introduced
- `test_ge_operator`, `05_array`, `test_fp_offset_cache` all pass

### Performance Impact

**Code Size**: No measurable change in typical code
- Test file `test_fp_offset_cache.c`: 1129 bytes (with and without cache)
- Reason: Register allocator prefers caller-saved registers (r0-r3, ip)

**Cache Effectiveness**: Limited by register allocator behavior
- The register allocator consistently chooses caller-saved registers for address computations
- These are short-lived values that don't benefit from caching
- Callee-saved registers (r4-r11) are rarely used for address calculations

---

## Key Findings

### Why Limited Impact?

1. **Register Allocator Behavior**: The register allocator (in `tccls.c`) prefers caller-saved registers (r0-r3, r12) for temporary address calculations because:
   - They're cheaper to use (no save/restore needed)
   - Address calculations are typically short-lived
   - Callee-saved registers are reserved for longer-lived values

2. **Addressing Modes**: ARM Thumb-2 has efficient addressing modes that often bypass address computation entirely:
   ```asm
   str.w r0, [r7, #-256]   ; Direct offset, no address computation needed
   ```

3. **Invalidation Frequency**: Even when callee-saved registers are used, they often get invalidated by:
   - Function calls (clears entire cache)
   - Scratch register allocation
   - Register spills

---

## Risk Analysis

### Implemented Approach (Callee-saved only)

| Risk | Status | Mitigation |
|------|--------|------------|
| Stale cache entries | ✅ Mitigated | Only cache callee-saved registers |
| Register pressure | ✅ Low | Uses existing registers, no reservation |
| Correctness bugs | ✅ None observed | Conservative invalidation + 480 tests pass |
| Code complexity | ✅ Low | ~20 lines of cache logic |

---

## Future Work

To achieve actual code size savings, consider:

1. **Dedicated cache register** (e.g., R11)
   - Reserve one callee-saved register exclusively for FP caching
   - Always copy computed addresses to cache register
   - More predictable behavior

2. **IR-level optimization**
   - Track address computation at IR level before register allocation
   - Coalesce redundant address computations
   - Let register allocator assign registers to coalesced values

3. **Enhanced register hints**
   - Hint to register allocator to prefer callee-saved registers for addresses
   - May increase register pressure but enable more caching

---

## Phased Implementation

### Phase 1: Conservative Improvement (Low Risk)

Only use the cache when both conditions are met:
1. Cache hit found
2. Cached register is the SAME as dest_reg

This catches the case where the same offset is computed multiple times into the same register (e.g., in a loop).

```c
if (tcc_state->opt_fp_offset_cache) {
  int cached_reg = tcc_fp_cache_lookup(cache, base_reg, frame_offset);
  if (cached_reg == dest_reg) {
    /* Same computation to same register - skip entirely! */
    return;
  }
}
```

### Phase 2: MOV-based Reuse (Medium Risk)

Allow cache hits with different dest_reg, emit MOV:

```c
if (cached_reg != PREG_REG_NONE && cached_reg != dest_reg) {
  /* Emit MOV to reuse cached value */
  ot_check(th_mov_reg(dest_reg, cached_reg, ...));
  return;
}
```

### Phase 3: Full Register Tracking (Higher Risk)

Implement comprehensive register write tracking to ensure cached values are always valid.

---

## Alternative Approach: Dedicated Cache Register

Instead of tracking which arbitrary register holds the value, reserve a dedicated register for FP offset caching:

### Option A: Use R11 as Cache Register

R11 is already marked as a scratch register. We could dedicate it for FP offset caching:

```c
#define FP_CACHE_REG ARM_R11

/* In tcc_machine_addr_of_stack_slot(): */
if (tcc_state->opt_fp_offset_cache) {
  int cached_reg = tcc_fp_cache_lookup(cache, base_reg, frame_offset);
  if (cached_reg == FP_CACHE_REG) {
    /* We have this offset in R11, just MOV to dest */
    if (dest_reg != FP_CACHE_REG) {
      ot_check(th_mov_reg(dest_reg, FP_CACHE_REG, ...));
    }
    return;
  }

  /* Miss - compute and store in BOTH dest_reg AND R11 for future use */
  compute_offset(dest_reg, ...);
  if (dest_reg != FP_CACHE_REG) {
    ot_check(th_mov_reg(FP_CACHE_REG, dest_reg, ...)); /* Cache it */
  }
  tcc_fp_cache_record(cache, FP_CACHE_REG, base_reg, frame_offset);
}
```

**Pros**:
- Simpler invalidation (only track one register)
- Predictable behavior

**Cons**:
- Extra MOV instruction on cache misses
- Reduces available registers by 1
- R11 pressure for other uses

### Option B: Opportunistic Caching (Recommended)

Cache into whichever register is used, but only reuse when the cached register is still live and unmodified:

```c
if (tcc_state->opt_fp_offset_cache) {
  FPOffsetCache *cache = &thumb_gen_state.fp_offset_cache;
  int cached_reg = tcc_fp_cache_lookup(cache, base_reg, frame_offset);

  if (cached_reg != PREG_REG_NONE) {
    /* Check if cached register is still holding our value */
    if (!register_was_written_since_cache(cached_reg)) {
      if (dest_reg == cached_reg) {
        return; /* Already in the right register! */
      }
      /* MOV is cheaper than SUB/ADD in most cases */
      ot_check(th_mov_reg(dest_reg, cached_reg, ...));
      return;
    }
    /* Stale entry - will be replaced below */
  }

  /* Compute and record */
  compute_offset_to_reg(dest_reg, base_reg, frame_offset);
  tcc_fp_cache_record(cache, dest_reg, base_reg, frame_offset);
}
```

---

## Tracking Register Writes

The challenge is knowing when a cached register has been overwritten. Options:

### Option 1: Instruction-Level Tracking (Complex)

Track every instruction's destination register and invalidate cache:

```c
/* In ot_check() or equivalent: */
if (instr_has_dest_reg(op)) {
  int dest = get_dest_reg_from_opcode(op);
  tcc_fp_cache_invalidate_reg(&cache, dest);
}
```

**Problem**: Requires parsing every opcode to extract destination register.

### Option 2: IR Instruction Boundaries (Simpler)

Clear/refresh cache at IR instruction boundaries:

```c
/* In gen_ir_instr() or equivalent: */
static void begin_ir_instruction(int idx) {
  /* Refresh cache validity based on liveness at this instruction */
  refresh_fp_cache_validity(idx);
}
```

### Option 3: Conservative Invalidation (Safest)

Invalidate cache entries aggressively:
- On any scratch register allocation
- On any store instruction
- On any call
- On any branch target

This reduces cache effectiveness but guarantees correctness.

---

## Recommended Implementation Order

1. **Immediate Win**: Remove `dest_reg == R_IP` restriction from cache recording
   - Always record computations, regardless of dest_reg
   - Keep lookup restriction for now (only hit when dest matches cached)

2. **Quick Follow-up**: Enable MOV-based reuse for caller-saved registers (R0-R3, R12)
   - These are frequently clobbered, so validity is clearer

3. **Later**: Extend to callee-saved registers with proper tracking

---

## Expected Impact

### Current (dest_reg == R_IP only)
- Cache hits: ~5-10% (only when dest happens to be R_IP)

### After Phase 1 (always record)
- Cache hits: ~20-30% (catches repeated same-dest computations)

### After Phase 2 (MOV reuse)
- Cache hits: ~60-80% (most redundant computations eliminated)

### Code Size Savings
- `sub.w rX, r7, #256`: 4 bytes
- `mov rX, rY`: 2 bytes (low regs) or 4 bytes (high regs)
- Savings per hit: 0-2 bytes code, 1 cycle execution

---

## Test Cases

### Test 1: Same Offset, Different Destinations
```c
void test1() {
  int arr[100];
  int *p1 = &arr[50];  // Computes fp-200 -> some reg
  int *p2 = &arr[50];  // Should reuse via MOV
}
```

### Test 2: Loop with Repeated Access
```c
void test2() {
  int arr[100];
  for (int i = 0; i < 10; i++) {
    arr[50] = i;  // Same offset each iteration
  }
}
```

### Test 3: Interleaved Invalidation
```c
void test3() {
  int arr[100];
  int *p1 = &arr[50];  // Cache: rX = fp-200
  func();              // Clears cache
  int *p2 = &arr[50];  // Must recompute (cache cleared)
}
```

---

## Summary

The FP offset cache improvement has been **implemented and tested**. The conservative approach (callee-saved registers only) ensures correctness but achieves limited code size reduction in practice.

### What Works
- ✅ Correctness: All 480 tests pass
- ✅ Framework: Cache infrastructure is in place
- ✅ Flag-based: Can be enabled/disabled via `-ffp-offset-cache`

### Limitations
- Register allocator prefers caller-saved registers for addresses
- Limited reuse opportunities in typical code patterns
- No measurable code size reduction in test cases

---

## Recommended Next Steps

### Option 1: IR-Level Address CSE (Most Effective)

**Approach**: Eliminate redundant address calculations at the IR level, before register allocation.

```
IR Before:
  %1 = ADDROF_LOCAL slot=-256    ; First access to arr[64]
  STORE %1, %2
  %3 = ADDROF_LOCAL slot=-256    ; Second access (REDUNDANT)
  STORE %3, %4

IR After:
  %1 = ADDROF_LOCAL slot=-256    ; Compute once
  STORE %1, %2
  STORE %1, %4                   ; Reuse %1
```

**Pros**:
- Lets register allocator handle lifetimes properly
- No register validity tracking needed at codegen
- Can coalesce across basic blocks

**Implementation**:
1. Add IR pass in `tccir.c` that identifies duplicate `ADDROF_LOCAL` with same offset
2. Replace duplicates with reference to first computation
3. Run before register allocation

### Option 2: Caller-Saved Register Caching with Liveness

**Approach**: Cache caller-saved registers (r0-r3, r12) but use liveness info to verify validity.

```c
if (tcc_state->opt_fp_offset_cache) {
  int cached_reg = tcc_fp_cache_lookup(cache, base_reg, frame_offset);
  if (cached_reg != PREG_REG_NONE) {
    /* Check if cached_reg is still live at current instruction */
    int instr_idx = tcc_state->ir->codegen_instruction_idx;
    uint32_t live = tcc_ls_compute_live_regs(&tcc_state->ir->ls, instr_idx);
    if (live & (1u << cached_reg)) {
      /* Register is still holding a value - might be our cached value */
      ot_check(th_mov_reg(dest_reg, cached_reg, ...));
      return;
    }
  }
}
```

**Risk**: Liveness says "register holds *something*", not "holds *our* cached value".

### Option 3: Dedicated Cache Register (Simplest)

**Approach**: Reserve R11 exclusively for FP offset caching.

```c
#define FP_CACHE_REG 11

/* On address computation: */
compute_offset(dest_reg, ...);
if (dest_reg != FP_CACHE_REG) {
  ot_check(th_mov_reg(FP_CACHE_REG, dest_reg, ...));  /* Cache copy */
}
tcc_fp_cache_record(cache, FP_CACHE_REG, base_reg, offset);

/* On future access: */
if (cached_reg == FP_CACHE_REG) {
  ot_check(th_mov_reg(dest_reg, FP_CACHE_REG, ...));  /* Reuse */
}
```

**Pros**:
- Simple and predictable
- Only one register to track for invalidation

**Cons**:
- Extra MOV on first use (cache miss)
- Reduces available registers

### Option 4: Keep as Infrastructure

**Approach**: Leave current implementation as a foundation, focus effort elsewhere.

The FP offset cache has diminishing returns because:
1. ARM Thumb-2 addressing modes are efficient (`str.w r0, [r7, #-256]`)
2. Register pressure is the bigger issue
3. Better wins available in other optimizations

**Recommendation**: Mark as "infrastructure complete" and prioritize:
- IR-level optimizations (constant folding, dead code elimination)
- Better register allocation hints
- Literal pool optimization

---

## Decision Matrix

| Approach | Effort | Correctness Risk | Expected Savings |
|----------|--------|-----------------|------------------|
| IR-Level CSE | Medium | Low | High |
| Caller-saved + Liveness | Medium | Medium | Medium |
| Dedicated Cache Reg | Low | Low | Medium |
| Keep as Infrastructure | None | None | None |

**Recommended**: Start with Option 3 (Dedicated Cache Register) for quick validation, then consider Option 1 (IR-Level CSE) for long-term solution.

2. Or move optimization to IR level before register allocation
3. Or enhance register allocator to prefer callee-saved registers for addresses

The optimization is safe to keep enabled (default with `-O1`) as it causes no regressions and may help in specific high-register-pressure scenarios.
3. Measure impact before adding complexity of full register tracking
