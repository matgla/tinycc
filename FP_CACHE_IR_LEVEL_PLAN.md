# IR-Level FP Offset Caching Plan

## Current Architecture

Currently, stack access works like this:

```
C Code:              arr[0] = 1;  arr[1] = 2;
                     ↓
TCC IR:              is_local=1, offset=-256   is_local=1, offset=-252
                     ↓
Backend (arm-thumb): sub r0, fp, #256          sub r0, fp, #252
```

The backend generates address computation for EVERY access.

## Proposed IR-Level Optimization

Add explicit address computation to the IR:

```
C Code:              arr[0] = 1;  arr[1] = 2;
                     ↓
TCC IR (optimized):  
  v10 = LOCAL_ADDR(-256)     ← Compute once
  STORE v10, #1
  
  v11 = LOCAL_ADDR(-252)     ← Compute once  
  STORE v11, #2
```

But this requires significant changes to IR generation.

## Simpler Alternative: Per-Offset Caching

For each unique stack offset, create the address computation once and reuse:

```
C Code:              arr[0] = 1;  arr[0] = 2;  // Same offset!
                     ↓
TCC IR (optimized):
  v10 = LOCAL_ADDR(-256)     ← Compute once
  STORE v10, #1
  STORE v10, #2              ← Reuse v10!
```

## Implementation Strategy

1. **Track address computations** in `tccir.c` during IR generation
2. **Map offsets to vregs** - maintain a hash table: `offset → vreg`
3. **Reuse vregs** for the same offset within a function
4. **Backend generates** `sub rd, fp, #offset` only once per vreg

## Data Structures

```c
typedef struct FPOffsetCacheEntry {
    int offset;          /* Stack offset from FP */
    int vreg;            /* Virtual register holding the address */
    int valid;           /* Is this entry valid? */
} FPOffsetCacheEntry;

#define FP_OFFSET_CACHE_SIZE 16

typedef struct TCCIRState {
    /* ... existing fields ... */
    
    /* Per-function FP offset cache for CSE */
    FPOffsetCacheEntry fp_offset_cache[FP_OFFSET_CACHE_SIZE];
    int fp_offset_cache_count;
} TCCIRState;
```

## Algorithm

```c
IROperand get_local_address(TCCIRState *ir, int offset) {
    /* Check if we already have this offset cached */
    for (int i = 0; i < ir->fp_offset_cache_count; i++) {
        if (ir->fp_offset_cache[i].offset == offset) {
            /* Hit! Return existing vreg */
            return irop_make_vreg(ir->fp_offset_cache[i].vreg, IROP_BTYPE_INT32);
        }
    }
    
    /* Miss - create new address computation */
    int new_vreg = tcc_ir_new_vreg(ir, ...);
    
    /* Emit IR instruction to compute address */
    IROperand dest = irop_make_vreg(new_vreg, IROP_BTYPE_INT32);
    IROperand base = irop_make_stackoff(offset, ...);
    tcc_ir_emit_local_addr(ir, dest, base);
    
    /* Add to cache */
    if (ir->fp_offset_cache_count < FP_OFFSET_CACHE_SIZE) {
        ir->fp_offset_cache[ir->fp_offset_cache_count].offset = offset;
        ir->fp_offset_cache[ir->fp_offset_cache_count].vreg = new_vreg;
        ir->fp_offset_cache_count++;
    }
    
    return dest;
}
```

## New IR Opcode

```c
TCCIR_OP_LOCAL_ADDR  /* Compute address: dest = fp + offset */
```

## Backend Support

In `arm-thumb-gen.c`:
```c
case TCCIR_OP_LOCAL_ADDR:
    /* Generate: sub rd, fp, #offset */
    tcc_machine_addr_of_stack_slot(dest_reg, offset, is_param);
    break;
```

## Benefits

1. **Architecture independent** - Works for any backend (ARM, x86, etc.)
2. **No register reservation** - Uses normal register allocation
3. **Natural CSE** - LLVM/GCC do similar optimizations
4. **Composable** - Works with other IR optimizations

## Challenges

1. **IR changes** - Need new opcode and tracking
2. **Invalidation** - Must clear cache at function calls (callee may modify stack)
3. **Liveness** - Need proper liveness analysis for the new vregs
4. **Testing** - Extensive testing required

## Implementation Phases

### Phase 1: Add IR opcode and tracking (1-2 hours)
- Add `TCCIR_OP_LOCAL_ADDR` to opcode enum
- Add cache data structures to TCCIRState
- Add helper functions for cache management

### Phase 2: Modify IR generation (2-3 hours)
- Track address computations in `tccir.c`
- Reuse vregs for same offsets
- Emit `LOCAL_ADDR` instructions

### Phase 3: Backend support (1 hour)
- Handle `TCCIR_OP_LOCAL_ADDR` in `arm-thumb-gen.c`
- Generate efficient address computation

### Phase 4: Testing (2-3 hours)
- Test with various code patterns
- Measure code size improvements
- Ensure no regressions

## Expected Results

For `test_fp_offset_cache.c`:
- **Before**: 12 `sub fp, #offset` instructions
- **After**: ~2-4 `sub fp, #offset` instructions (unique offsets only)
- **Savings**: ~67% reduction in address computations

## Current Status

The backend-level cache (R11-based) has been **abandoned** due to complexity.

This IR-level approach is the **recommended path forward** for a clean, maintainable implementation.
