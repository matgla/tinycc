# TACQuadruple SValue Pool Refactoring Plan

## Problem Statement

The current `TACQuadruple` struct embeds three full `SValue` structs directly:

```c
typedef struct TACQuadruple {
  int orig_index;
  TccIrOp op;
  SValue src1;    // 64 bytes
  SValue src2;    // 64 bytes
  SValue dest;    // 64 bytes
  int line_num;
} TACQuadruple;   // ~204 bytes total
```

**Issues:**
1. **Memory waste**: Only 45.6% of operations use all three SValue fields
2. **Cache inefficiency**: Large struct size means fewer instructions fit in cache
3. **Overhead**: Each instruction is ~204 bytes, with 192 bytes (94%) being SValue data

### Operation Field Usage Analysis

| Category | Operations | src1 | src2 | dest | Count |
|----------|-----------|------|------|------|-------|
| Binary ops | ADD, SUB, MUL, DIV, AND, OR, XOR, SHL, etc. | ✓ | ✓ | ✓ | 26 |
| Unary ops | NEG, NOT, CAST, SIGN_EXT, ZERO_EXT | ✓ | ✗ | ✓ | 8 |
| Load/Store | LOAD, STORE | ✓ | ✗ | ✓ | 4 |
| Control flow | JUMP, IJUMP | ✗ | ✗ | ✓ | 2 |
| Conditionals | JUMPIF, SETIF | ✓ | ✗ | ✓ | 2 |
| Calls | FUNCCALL, FUNCCALLVAL | varies | varies | varies | 5 |
| Returns | RETURNVOID | ✗ | ✗ | ✗ | 1 |
| Returns | RETURNVALUE | ✓ | ✗ | ✗ | 1 |
| Misc | NOP, LABEL, PHI | varies | varies | varies | 8 |

---

## Proposed Solution: SValue Pool with Index References

### New Data Structures

```c
/* Compact operand reference - replaces embedded SValue */
typedef struct IROperand {
  int16_t pool_index;   /* Index into SValue pool, or special value */
  uint16_t flags;       /* Operand flags (immediate, register, etc.) */
} IROperand;            /* 4 bytes vs 64 bytes */

/* Special pool_index values */
#define IR_OPERAND_NONE    (-1)  /* No operand (unused field) */
#define IR_OPERAND_VREG    (-2)  /* Virtual register only (vr in flags) */
#define IR_OPERAND_IMM8    (-3)  /* 8-bit immediate in flags */

/* Refactored TACQuadruple */
typedef struct TACQuadruple {
  int orig_index;
  TccIrOp op;
  IROperand src1;       /* 4 bytes */
  IROperand src2;       /* 4 bytes */
  IROperand dest;       /* 4 bytes */
  int line_num;
} TACQuadruple;         /* ~24 bytes (down from 204!) */

/* SValue pool stored separately */
typedef struct SValuePool {
  SValue *values;       /* Dynamic array of SValues */
  int count;            /* Number of used entries */
  int capacity;         /* Allocated capacity */
  /* Optional: hash table for deduplication */
  uint32_t *hash_table;
  int hash_size;
} SValuePool;
```

### Memory Savings Estimate

| Metric | Before | After | Savings |
|--------|--------|-------|---------|
| TACQuadruple size | 204 bytes | 24 bytes | 88% |
| 1000 instructions | 204 KB | 24 KB + pool | ~80-85% |
| Pool overhead | N/A | ~20-40 KB typical | - |
| **Net savings** | - | - | **~70-80%** |

---

## Implementation Phases

### Phase 1: Add SValue Pool Infrastructure

**Files to modify:** `tccir.h`, `tccir.c`

1. Add `SValuePool` struct definition to `tccir.h`
2. Add pool management functions:
   ```c
   void svalue_pool_init(SValuePool *pool);
   void svalue_pool_free(SValuePool *pool);
   int svalue_pool_add(SValuePool *pool, const SValue *sv);
   SValue *svalue_pool_get(SValuePool *pool, int index);
   ```
3. Add pool to `TCCIRState`:
   ```c
   typedef struct TCCIRState {
     // ... existing fields ...
     SValuePool svalue_pool;
   } TCCIRState;
   ```

### Phase 2: Add IROperand Type and Helpers

**Files to modify:** `tcc.h`, `tccir.h`

1. Define `IROperand` struct in `tcc.h`
2. Add helper macros/functions:
   ```c
   /* Create operand from pool index */
   IROperand ir_operand_from_pool(int pool_idx);

   /* Create "none" operand for unused fields */
   IROperand ir_operand_none(void);

   /* Check if operand is valid/used */
   bool ir_operand_is_valid(IROperand op);

   /* Resolve operand to SValue (may return static for NONE) */
   const SValue *ir_operand_resolve(TCCIRState *ir, IROperand op);
   ```

### Phase 3: Create Parallel TACQuadrupleCompact

**Strategy:** Keep old `TACQuadruple` during transition, add new compact version

```c
typedef struct TACQuadrupleCompact {
  int orig_index;
  TccIrOp op;
  IROperand src1;
  IROperand src2;
  IROperand dest;
  int line_num;
} TACQuadrupleCompact;
```

Add conversion functions:
```c
/* Convert compact to full (for codegen compatibility) */
void tac_expand(TCCIRState *ir, const TACQuadrupleCompact *compact, TACQuadruple *full);

/* Convert full to compact (for storage) */
void tac_compact(TCCIRState *ir, const TACQuadruple *full, TACQuadrupleCompact *compact);
```

### Phase 4: Modify tcc_ir_put() to Use Pool

**Files to modify:** `tccir.c`

1. Update `tcc_ir_put()` to:
   - Add SValues to pool instead of copying directly
   - Store pool indices in IROperand fields
   - Use `irop_config[]` to skip unused operands (set to `IR_OPERAND_NONE`)

2. Before (current):
   ```c
   if (irop_config[op].has_src1 == 1)
     q->src1 = *src1;
   else
     q->src1.vr = -1;
   ```

3. After (with pool):
   ```c
   if (irop_config[op].has_src1 == 1)
     q->src1 = ir_operand_from_pool(svalue_pool_add(&ir->svalue_pool, src1));
   else
     q->src1 = ir_operand_none();
   ```

### Phase 5: Update IR Optimization Passes

**Files to modify:** `tccir.c` (optimization functions)

Key functions to update:
- `tccir_dead_code_elimination()`
- `tccir_constant_folding()`
- `tccir_copy_propagation()`
- `tccir_resolve_virtual_regs()`

For each, change direct SValue access to use pool resolution:
```c
// Before
const SValue *src = &q->src1;

// After
const SValue *src = ir_operand_resolve(ir, q->src1);
```

### Phase 6: Update Code Generation Backend

**Files to modify:** `arm-thumb-gen.c`, and any other backends

The ARM backend heavily accesses `q->src1`, `q->src2`, `q->dest` directly. Options:

**Option A: Expand on demand (recommended for initial migration)**
```c
void gen_arm_instruction(TCCIRState *ir, const TACQuadrupleCompact *qc) {
  TACQuadruple q_expanded;
  tac_expand(ir, qc, &q_expanded);
  // Use q_expanded as before - minimal changes to codegen
}
```

**Option B: Update all access sites (long-term)**
```c
// Change all direct accesses
const SValue *src1 = ir_operand_resolve(ir, q->src1);
```

### Phase 7: Optional - SValue Deduplication

Add hash-based deduplication to the pool to avoid storing identical SValues:

```c
int svalue_pool_add(SValuePool *pool, const SValue *sv) {
  uint32_t hash = svalue_hash(sv);
  int existing = svalue_pool_find(pool, sv, hash);
  if (existing >= 0)
    return existing;  // Reuse existing entry
  // ... add new entry ...
}
```

Expected additional savings: 10-30% depending on code patterns.

---

## Migration Strategy

### Step 1: Non-Breaking Foundation
- Add `SValuePool` alongside existing code
- Add `IROperand` type definitions
- Add helper functions
- **No behavior changes yet**

### Step 2: Parallel Storage
- Store both formats during IR building
- Validate compact format produces identical results
- Add test coverage comparing old vs new

### Step 3: Switch IR Storage
- Change `TCCIRState.instructions` to use compact format
- Keep expansion in codegen for compatibility
- Measure memory savings

### Step 4: Optimize Codegen (optional)
- Update backends to work with pool directly
- Remove expansion step
- Maximum performance

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Correctness bugs in conversion | Medium | High | Parallel validation, extensive tests |
| Performance regression | Low | Medium | Expand-on-demand preserves old codegen |
| Pointer invalidation | Medium | High | Pool never shrinks; use indices not pointers |
| Thread safety | Low | Low | TCC is single-threaded per compilation |

---

## Files to Modify (Summary)

| File | Changes |
|------|---------|
| `tcc.h` | Add `IROperand` struct |
| `tccir.h` | Add `SValuePool`, `TACQuadrupleCompact`, helper prototypes |
| `tccir.c` | Pool implementation, update `tcc_ir_put()`, update optimizations |
| `arm-thumb-gen.c` | Add expansion wrapper or update access sites |

---

## Testing Strategy

1. **Unit tests**: Pool add/get/dedup operations
2. **Integration tests**: Compile existing test suite with new IR storage
3. **Memory tests**: Verify actual memory reduction matches estimates
4. **Regression tests**: Binary output must be identical before/after
5. **Benchmark**: Compilation speed should not regress significantly

---

## Success Metrics

- [ ] TACQuadruple size reduced from ~204 to ~24 bytes
- [ ] Overall IR memory usage reduced by 70%+ for typical code
- [ ] All existing tests pass
- [ ] Compilation speed within 5% of baseline
- [ ] No increase in binary output size
