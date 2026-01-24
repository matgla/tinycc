# Dual Pool Migration TODO

## Status: Phase 4 Complete - All Sync Points Audited

All 466 tests pass. All optimization passes and infrastructure functions audited for proper sync.

---

## ✅ Completed

### Infrastructure (Phase 0-3)
- [x] `IROperand` type with tag encoding in `tccir_operand.h`
- [x] Pool management: `pool_i64`, `pool_f64`, `pool_symref`
- [x] `tcc_ir_pools_init()` / `tcc_ir_pools_free()`
- [x] `tcc_ir_pool_add_i64()` / `tcc_ir_pool_add_f64()` / `tcc_ir_pool_add_symref()`
- [x] `svalue_to_iroperand()` - converts SValue to tagged IROperand
- [x] `iroperand_to_svalue()` - expands IROperand back to SValue
- [x] `iroperand_pool` parallel to `svalue_pool` in TCCIRState
- [x] IROperand accessor functions in `tccir.h`:
  - `tcc_ir_op_get_dest_irop()`
  - `tcc_ir_op_get_src1_irop()`
  - `tcc_ir_op_get_src2_irop()`

### Synchronization Layer (Phase 3.5)
- [x] `tcc_ir_sync_operand()` - write single operand to both pools
- [x] `tcc_ir_resync_operand()` - resync from svalue_pool → iroperand_pool
- [x] `tcc_ir_sync_quad()` - sync all operands of a TACQuadruple
- [x] `tcc_ir_svalue_pool_add()` populates both pools
- [x] `tcc_ir_backpatch()` updates both pools
- [x] `tcc_ir_backpatch_first()` updates both pools

---

## 🔄 In Progress: Optimization Pass Porting

**Current approach**: Optimization passes continue to use SValue accessors for reading.
After modifications, call `tcc_ir_resync_operand()` to sync to iroperand_pool.

### Tier 1: Simple Optimization Passes

| Function | File:Line | Status | Notes |
|----------|-----------|--------|-------|
| `tcc_ir_dead_code_elimination()` | tccir.c:4091 | ✅ Uses SValue | Marks NOPs only |
| `tcc_ir_dead_store_elimination()` | tccir.c:4170 | ✅ Uses SValue | Marks NOPs only |
| `tcc_ir_bool_idempotent()` | tccir.c:4399 | ✅ Synced | `tcc_ir_resync_operand` for src2 |

### Tier 2: Copy/Constant Propagation

| Function | File:Line | Status | Notes |
|----------|-----------|--------|-------|
| `tcc_ir_copy_propagation()` | tccir.c:5453 | ✅ Synced | `tcc_ir_resync_operand` after src1/src2 |
| `tcc_ir_constant_propagation()` | tccir.c:~4800 | ✅ Synced | Multiple `tcc_ir_resync_operand` calls |
| `tcc_ir_tmp_constant_propagation()` | tccir.c:~5300 | ✅ Synced | `tcc_ir_resync_operand` after src1/src2 |

### Tier 3: CSE and Store Optimizations

| Function | File:Line | Status | Notes |
|----------|-----------|--------|-------|
| `tcc_ir_bool_cse()` | tccir.c:~4350 | ✅ Synced | `tcc_ir_resync_operand` after ASSIGN conversion |
| `tcc_ir_arithmetic_cse()` | tccir.c:~6200 | ✅ Synced | `tcc_ir_resync_operand` after src1/src2 |
| `tcc_ir_store_load_forwarding()` | tccir.c:~5800 | ✅ Synced | `tcc_ir_resync_operand` after ASSIGN conversion |
| `tcc_ir_redundant_store_elimination()` | tccir.c:~6000 | ✅ OK | Only sets op=NOP |
| `tcc_ir_bool_simplification()` | tccir.c:~4650 | ✅ Synced | `tcc_ir_resync_operand` calls |
| `tcc_ir_return_value_optimization()` | tccir.c:~4550 | ✅ Synced | Multiple sync calls |

### Helper Functions

| Function | File:Line | Status | Notes |
|----------|-----------|--------|-------|
| `same_bool_operands()` | tccir.c:~4280 | ✅ Uses SValue | Reads only |

### Swap/Modification Sites (Audit Complete)

| Pattern | Lines | Status | Notes |
|---------|-------|--------|-------|
| `src1->vr = -1` | 4465, 4478, 4499, 5000, 5194 | ✅ Synced | All have `tcc_ir_resync_operand` |
| `src2->vr = -1` | 4481, 4541, 5004, 5198 | ✅ Synced | All have `tcc_ir_resync_operand` |
| `src->c.i = 0` | 5089, 5187 | ✅ Synced | Constant propagation fold |
| Operand swap | 4998, 5193 | ✅ Synced | Swap + sync both operands |
| `tcc_ir_backpatch()` | 7719 | ✅ Synced | Manual iroperand_pool update |

---

## ✅ Audited: Infrastructure

### Tier 4: Infrastructure Functions (Complete)

| Function | File | Status | Notes |
|----------|------|--------|-------|
| `tcc_ir_put()` | tccir.c | ✅ Synced | Main instruction emission - synced via `tcc_ir_svalue_pool_add` |
| `tcc_ir_liveness_analysis()` | tccir.c:2691 | ✅ Reads-only | Reads via SValue accessors, writes to IRLiveInterval structs |
| `tcc_ir_assign_physical_register()` | tccir.c:2858 | ✅ N/A | Writes to IRLiveInterval.allocation, not operand pools |
| `tcc_ir_fill_registers()` | tccir.c:3920 | ✅ Codegen-only | Modifies SValue during codegen - after all opts, no sync needed |

### Tier 5: Code Generation (Port Last)

| Function | File | Priority | Notes |
|----------|------|----------|-------|
| `tcc_ir_generate_code()` | tccir.c | Low | Main codegen loop - can use IROperand after all opts done |
| `tcc_gen_machine_*()` | arm-thumb-gen.c | Low | Per-instruction codegen |
| Peephole patterns | arm-thumb-gen.c | Low | Optimization within codegen |

---

## ⚠️ Key Learnings / Gotchas

### 1. Op Change Timing Issue
When changing `q->op` (e.g., ADD → ASSIGN), the operand layout changes.
Must sync operands BEFORE or use the correct slot indices.

```c
// WRONG: After op change, src2 slot doesn't exist for ASSIGN
q->op = TCCIR_OP_ASSIGN;
tcc_ir_resync_operand(ir, i, 2);  // Returns early - ASSIGN has no src2!

// RIGHT: Sync while we still know the slot mapping
*src1 = new_value;
tcc_ir_resync_operand(ir, i, 1);
*src2 = cleared;  
tcc_ir_resync_operand(ir, i, 2);  // Still valid slot for original op
q->op = TCCIR_OP_ASSIGN;          // Change op AFTER sync
```

### 2. Read from svalue_pool During Optimizations
Optimization passes MUST read from `svalue_pool` (via SValue accessors) because:
- Other optimization passes may have modified svalue_pool
- iroperand_pool is only updated via sync functions
- Reading stale IROperand data causes incorrect optimizations

### 3. IROperand.vr Encoding (Enhanced)
The `.vr` field in IROperand now has:
- Bits 0-19: vreg position
- Bits 20-22: tag (IROP_TAG_*)
- Bits 23-24: flags (IROP_FLAG_LVAL, IROP_FLAG_LLOCAL)
- Bits 25-27: **btype** (compressed VT_BTYPE)
- Bits 28-31: vreg type

Use `irop_get_vreg()` to extract clean vreg, `irop_get_btype()` for type info.

### 4. ~~Codegen Cannot Use iroperand_to_svalue~~ ✅ FIXED
**Enhanced** IROperand encoding now preserves:
- ✅ `type.t` via compressed btype (3 bits: INT32/INT64/FLOAT32/FLOAT64/STRUCT/FUNC)
- ✅ `VT_LOCAL` via IROP_TAG_STACKOFF
- ✅ `VT_LLOCAL` via IROP_FLAG_LLOCAL
- ✅ Symbol references via IROP_TAG_SYMREF + pool

**Helper functions available** (for codegen port):
- `tcc_ir_codegen_get_operand()` - reads from iroperand_pool + fill_registers
- `tcc_ir_codegen_get_src1/src2/dest()` - convenience wrappers
- `vt_btype_to_irop_btype()` / `irop_btype_to_vt_btype()` - type conversion

---

## 📁 Files Modified

| File | Changes |
|------|---------|
| `tccir_operand.h` | IROperand type, btype field, encoding/decoding helpers |
| `tccir_operand.c` | Pool management, svalue_to_iroperand with btype, iroperand_to_svalue |
| `tccir.h` | TCCIRState with dual pools, IROperand accessor declarations |
| `tccir.c` | Sync functions, optimization passes with resync calls, codegen helpers |

---

## 🎯 Next Steps

1. ~~**Verify all sync calls are correct**~~ ✅ Done
2. ~~**Enhance IROperand encoding**~~ ✅ Done - added btype, LLOCAL flag
3. **Port codegen to use IROperand** - retry with enhanced encoding
4. **Add debug validation** - optional sync check between pools (nice-to-have)
5. **Remove svalue_pool** - final cleanup once codegen migrated

---

## Test Commands

```bash
# Full rebuild and test
cd /Users/mateusz/repos/tinycc
make clean && make -j4 && cd tests/ir_tests && python3 runner.py

# Quick test with optimization
./armv8m-tcc -c -O1 tests/ir_tests/01_hello_world.c -o /tmp/test.o

# Dump IR to verify optimizations
./armv8m-tcc -c -dump-ir tests/ir_tests/01_hello_world.c -o /tmp/test.o
```
