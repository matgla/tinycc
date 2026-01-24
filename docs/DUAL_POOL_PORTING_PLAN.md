# Dual Pool Porting Plan: SValue Pool + IROperand Pool

## Overview

This document describes a safe step-by-step approach to port from the current `SValue*` based system to `IROperand` (u64-tagged), while maintaining **two synchronized pools** during the transition.

### Current State
- `SValue` pool exists in `TCCIRState` (`svalue_pool`, `svalue_pool_count`, `svalue_pool_capacity`)
- `IRQuadCompact` stores `operand_base` (index into svalue_pool)
- ~90 places still read from `ir->instructions[]` (old TACQuadruple array)
- Accessors like `tcc_ir_get_dest()`, `tcc_ir_get_src1()`, `tcc_ir_get_src2()` return `SValue*`

### Target State
- `IROperand` (u64-tagged) as the canonical operand representation
- **Separate pools** for cache efficiency: `pool_i64`, `pool_f64`, `pool_symref`
- SValue pool kept as a "shadow" for backward compatibility during porting
- Full synchronization between pools after every write

---

## Key Design Principle: Pool Synchronization

Since two pools coexist, every write operation must update both:

```c
// After any operand modification:
1. Write to IROperand (new system)
2. Expand IROperand → SValue and write to svalue_pool (old system)
```

This is achieved via a **sync layer** that wraps all operand writes.

---

## Phase 0: Define IROperand Infrastructure ✅ IMPLEMENTED

### 0.1 IROperand type with 3-bit tags

```c
typedef uint64_t IROperand;

/* 3-bit tags (8 types max) - allows inline F32 */
#define IROP_TAG_IMM32    0 /* payload: signed 32-bit immediate */
#define IROP_TAG_VREG     1 /* payload: vreg id */
#define IROP_TAG_STACKOFF 2 /* payload: signed 32-bit FP-relative offset */
#define IROP_TAG_F32      3 /* payload: 32-bit float bits (inline!) */
#define IROP_TAG_I64      4 /* payload: index into pool_i64[] */
#define IROP_TAG_F64      5 /* payload: index into pool_f64[] */
#define IROP_TAG_SYMREF   6 /* payload: index into pool_symref[] */
#define IROP_TAG_NONE     7 /* sentinel for unused operand */

#define IROP_TAG_MASK       7
#define IROP_PAYLOAD_SHIFT  3
```

**Key insight**: F32 fits inline (32 bits payload), only I64/F64/SYMREF need pools.

### 0.2 Separate pools for cache efficiency

```c
/* In TCCIRState: */
int64_t *pool_i64;          /* 64-bit integer constants */
int pool_i64_count;
int pool_i64_capacity;

uint64_t *pool_f64;         /* 64-bit double bits */
int pool_f64_count;
int pool_f64_capacity;

IRPoolSymref *pool_symref;  /* symbol references */
int pool_symref_count;
int pool_symref_capacity;
```

### 0.3 Pool entry type for SYMREF

```c
typedef struct IRPoolSymref {
    struct Sym *sym;
    int32_t addend;
    uint32_t flags;  /* IRPOOL_SYMREF_LVAL, IRPOOL_SYMREF_LOCAL */
} IRPoolSymref;
```

### 0.4 Pool management functions ✅ IMPLEMENTED

```c
void tcc_ir_pools_init(TCCIRState *ir);
void tcc_ir_pools_free(TCCIRState *ir);
uint32_t tcc_ir_pool_add_i64(TCCIRState *ir, int64_t val);
uint32_t tcc_ir_pool_add_f64(TCCIRState *ir, uint64_t bits);
uint32_t tcc_ir_pool_add_symref(TCCIRState *ir, Sym *sym, int32_t addend, uint32_t flags);
```

---

## Phase 1: Conversion Functions (NEXT)

These functions form the **synchronization layer** between pools.

### 1.1 SValue → IROperand conversion

```c
/* Convert SValue to IROperand, adding to pool if needed */
IROperand svalue_to_iroperand(TCCIRState *ir, const SValue *sv);
```

Logic:
1. If `sv == NULL` → return `IROP_NONE`
2. If pure vreg (no const, no sym, no lval) → return `irop_make_vreg(sv->vr)`
3. If `VT_CONST` with small immediate (fits signed 32-bit) and no symbol → return `irop_make_imm32(sv->c.i)`
4. If `VT_LOCAL` stackoff without lval → return `irop_make_stackoff(sv->c.i)`
5. If float type → return `irop_make_f32(float_bits)`
6. Otherwise → add to appropriate pool:
   - I64 for 64-bit constants → `irop_make_i64(pool_idx)`
   - F64 for doubles → `irop_make_f64(pool_idx)`
   - SYMREF for symbol references → `irop_make_symref(pool_idx)`

### 1.2 IROperand → SValue expansion

```c
/* Expand IROperand back to SValue (for backward compatibility) */
void iroperand_to_svalue(TCCIRState *ir, IROperand op, SValue *out);
```

Logic (reverse of above):
1. `IROP_TAG_NONE` → clear `out` with `svalue_init(out)`
2. `IROP_TAG_VREG` → `out->vr = irop_get_vreg(op)`, `out->r = VT_CONST`
3. `IROP_TAG_IMM32` → `out->r = VT_CONST`, `out->c.i = irop_get_imm32(op)`
4. `IROP_TAG_STACKOFF` → `out->r = VT_LOCAL`, `out->c.i = irop_get_stackoff(op)`
5. `IROP_TAG_F32` → `out->r = VT_CONST`, `out->c.f = bits_to_float(irop_get_f32(op))`
6. `IROP_TAG_I64` → `out->c.i = ir->pool_i64[idx]`
7. `IROP_TAG_F64` → `out->c.d = bits_to_double(ir->pool_f64[idx])`
8. `IROP_TAG_SYMREF` → populate sym, addend, flags from `ir->pool_symref[idx]`

---

## Phase 3: Synchronized Write Helpers

### 3.1 Unified operand write function

```c
/* Write operand to BOTH pools - keeps them in sync */
void tcc_ir_write_operand(TCCIRState *ir, int instr_idx,
                          int operand_slot,  /* 0=dest, 1=src1, 2=src2 */
                          const SValue *sv)
{
    IRQuadCompact *cq = &ir->compact_instructions[instr_idx];
    IRQuadNew *nq = &ir->new_instructions[instr_idx];  /* if parallel arrays */

    /* Convert to IROperand (may add to iroperand_pool) */
    IROperand irop = svalue_to_iroperand(ir, sv);

    /* Write to new IROperand-based instruction */
    switch (operand_slot) {
        case 0: nq->dest = irop; break;
        case 1: nq->src1 = irop; break;
        case 2: nq->src2 = irop; break;
    }

    /* Write to old SValue pool for backward compatibility */
    int pool_off = cq->operand_base;
    const IRRegistersConfig *cfg = &irop_config[cq->op];

    int slot_idx = 0;
    if (operand_slot == 0 && cfg->has_dest) {
        ir->svalue_pool[pool_off + slot_idx] = *sv;
    }
    if (cfg->has_dest) slot_idx++;

    if (operand_slot == 1 && cfg->has_src1) {
        ir->svalue_pool[pool_off + slot_idx] = *sv;
    }
    if (cfg->has_src1) slot_idx++;

    if (operand_slot == 2 && cfg->has_src2) {
        ir->svalue_pool[pool_off + slot_idx] = *sv;
    }
}
```

### 3.2 Bulk writeback after modification

```c
/* After modifying a TACQuadruple, sync both pools */
void tcc_ir_sync_pools(TCCIRState *ir, int instr_idx, const TACQuadruple *q)
{
    IRQuadCompact *cq = &ir->compact_instructions[instr_idx];
    const IRRegistersConfig *cfg = &irop_config[cq->op];

    if (cfg->has_dest)
        tcc_ir_write_operand(ir, instr_idx, 0, &q->dest);
    if (cfg->has_src1)
        tcc_ir_write_operand(ir, instr_idx, 1, &q->src1);
    if (cfg->has_src2)
        tcc_ir_write_operand(ir, instr_idx, 2, &q->src2);
}
```

---

## Phase 4: Incremental Porting Pattern

For each function that modifies operands:

### 4.1 Identify modification sites

Search for patterns like:
```c
q->dest.vr = ...
q->src1.r = ...
ir->svalue_pool[...] = ...
```

### 4.2 Replace with synchronized writes

**Before:**
```c
TACQuadruple *q = &ir->instructions[i];
q->dest.vr = new_vr;
tcc_ir_writeback_quad(ir, i, q);  /* only writes to svalue_pool */
```

**After:**
```c
TACQuadruple q;
tcc_ir_expand_quad(ir, i, &q);
q.dest.vr = new_vr;
tcc_ir_sync_pools(ir, i, &q);  /* writes to BOTH pools */
```

---

## Phase 5: Porting Order (Recommended)

### Tier 1: Simple optimization passes (lowest risk)
1. `tcc_ir_dead_code_elimination()` - marks NOPs, minimal operand writes
2. `tcc_ir_dead_store_elimination()` - marks NOPs
3. `tcc_ir_bool_idempotent()` - simple vr replacements

### Tier 2: Copy/constant propagation (operand rewrites)
4. `tcc_ir_copy_propagation()` - replaces vreg references
5. `tcc_ir_constant_propagation()` - replaces with constants
6. `tcc_ir_tmp_constant_propagation()` - similar

### Tier 3: Complex optimizations
7. `tcc_ir_arithmetic_cse()` - vr replacements
8. `tcc_ir_bool_cse()` - vr replacements
9. `tcc_ir_store_load_forwarding()` - may rewrite operands
10. `tcc_ir_redundant_store_elimination()`

### Tier 4: Infrastructure functions
11. `tcc_ir_put()` - instruction emission (already writes to svalue_pool)
12. `tcc_ir_liveness_analysis()` - reads, may update intervals
13. `tcc_ir_assign_registers()` - writes allocation results

### Tier 5: Code generation (highest risk, last)
14. `tcc_ir_generate_code()` - main codegen loop
15. Peephole optimizations within codegen

---

## Phase 6: Verification Strategy

After each ported function:

### 6.1 Add sync validation (debug builds)
```c
#ifdef DEBUG
void tcc_ir_validate_pool_sync(TCCIRState *ir, int instr_idx)
{
    /* Expand from both pools and compare */
    SValue sv_old, sv_new;

    /* From svalue_pool via compact_instructions */
    tcc_ir_expand_quad(ir, instr_idx, &q_old);

    /* From iroperand_pool via new_instructions */
    tcc_ir_expand_from_iroperand(ir, instr_idx, &q_new);

    assert(memcmp(&q_old.dest, &q_new.dest, sizeof(SValue)) == 0);
    assert(memcmp(&q_old.src1, &q_new.src1, sizeof(SValue)) == 0);
    assert(memcmp(&q_old.src2, &q_new.src2, sizeof(SValue)) == 0);
}
#endif
```

### 6.2 Run full test suite
```bash
make clean && make && make test -j32
```

---

## Phase 7: Final Cleanup (After All Porting Complete)

1. Remove `SValue *svalue_pool` from TCCIRState
2. Remove `IRQuadCompact` - replaced by `IRQuadNew`
3. Update all accessors to use `IROperand` directly
4. Remove conversion functions (or keep as debug aids)

---

## Memory Layout Comparison

| Structure | Size | Notes |
|-----------|------|-------|
| `TACQuadruple` | ~160 bytes | Full SValue × 3 embedded |
| `IRQuadCompact` | 16 bytes | pool index only |
| `IRQuadNew` | 40 bytes | 3 × IROperand (8 bytes each) + metadata |
| `SValue` | ~56 bytes | Per operand in pool |
| `int64_t` (pool_i64) | 8 bytes | I64 constant |
| `uint64_t` (pool_f64) | 8 bytes | F64 bits |
| `IRPoolSymref` | ~16 bytes | Symbol reference |

**Estimated savings**: 60-80% memory reduction once migration complete.

---

## Implementation Checklist

- [x] Phase 0: Define IROperand infrastructure
  - [x] Add `IROperand` type with 3-bit tags (tccir.h)
  - [x] Add encoding/decoding helpers (inline functions)
  - [x] Add `IRPoolSymref` struct for symbol references
  - [x] Add separate pool storage to TCCIRState (`pool_i64`, `pool_f64`, `pool_symref`)

- [x] Phase 1: Pool management
  - [x] Implement `tcc_ir_pools_init()` / `tcc_ir_pools_free()`
  - [x] Implement `tcc_ir_pool_add_i64()` / `tcc_ir_pool_add_f64()` / `tcc_ir_pool_add_symref()`
  - [ ] Define `IRQuadNew` struct (optional - may use existing IRQuadCompact)

- [x] Phase 2: Conversion functions ✅
  - [x] Implement `svalue_to_iroperand()` - converts SValue to tagged IROperand
  - [x] Implement `iroperand_to_svalue()` - expands IROperand back to SValue

- [x] Phase 3: Synchronized write helpers ✅
  - [x] Define `TACQuadruple` struct (expanded instruction form)
  - [x] Implement `tcc_ir_expand_quad()` - expand IRQuadCompact to TACQuadruple
  - [x] Implement `tcc_ir_writeback_quad()` - write TACQuadruple back to svalue_pool
  - [x] Implement `tcc_ir_sync_operand()` - write single operand to both pools
  - [x] Implement `tcc_ir_sync_quad()` - sync all operands to both pools

- [x] Phase 3.5: Parallel IROperand population ✅
  - [x] Add `iroperand_pool` array to TCCIRState (parallel to svalue_pool)
  - [x] Modify `tcc_ir_svalue_pool_add()` to populate both pools
  - [x] Add IROperand accessor functions (`tcc_ir_get_dest_irop()`, etc.)
  - [x] All 466 tests passing with dual-pool population

- [ ] Phase 4-5: Port functions (see Tier list above)
  - [ ] Tier 1: DCE, DSE, bool_idempotent
  - [ ] Tier 2: copy_propagation, constant_propagation
  - [ ] Tier 3: arithmetic_cse, bool_cse, store_load_forwarding
  - [ ] Tier 4: tcc_ir_put, liveness_analysis, assign_registers
  - [ ] Tier 5: tcc_ir_generate_code, peepholes

- [ ] Phase 6: Validation
  - [ ] Debug sync validation
  - [ ] All tests passing

- [ ] Phase 7: Cleanup
  - [ ] Remove old svalue_pool
  - [ ] Finalize IROperand-only storage
