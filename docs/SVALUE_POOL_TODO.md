# SValue Pool Migration TODO

## Completed

- [x] Phase 1: Pool infrastructure in tccir.h/tccir.c
  - `SValue *svalue_pool`, `svalue_pool_count`, `svalue_pool_capacity`
  - `IRQuadCompact` struct (16 bytes vs ~160 bytes TACQuadruple)
  - `IRRegistersConfig` moved to header
  - Inline accessor functions: `tcc_ir_op_get_dest()`, `tcc_ir_op_get_src1()`, `tcc_ir_op_get_src2()`, `ir_op_slot_count()`

- [x] Phase 2: Parallel storage in `tcc_ir_put()`
  - Stores to both TACQuadruple and pool/IRQuadCompact
  - Handles coalescing by updating pool entries

- [x] Phase 3: IRQuadCompact array
  - `compact_instructions` array parallel to `instructions`
  - Auto-resize on growth
  - Proper cleanup in `tcc_ir_release_block()`

- [x] Phase 4: Migration helpers
  - `tcc_ir_expand_quad()` - expand compact to full TACQuadruple
  - `tcc_ir_writeback_quad()` - write modified operands back to pool

- [x] Phase 4.5: Simplified dead code/store elimination
  - Changed from array compaction to NOP marking
  - No jump target updates needed (indices stay stable)
  - Much simpler code, works seamlessly with pool-based storage
  - `tcc_ir_dead_code_elimination()` now marks unreachable as NOP
  - `tcc_ir_dead_store_elimination()` now marks dead stores as NOP
  - **All 466 tests passing**

- [x] Phase 4.6: Writeback calls added to optimization passes
  - All optimization passes now call `tcc_ir_writeback_quad()` after modifications
  - Keeps pool in sync with `instructions[]` array
  - Still READS from `ir->instructions[]` (migration incomplete)

**All 466 tests passing with parallel storage infrastructure in place.**

## Design Decision: NOP Marking vs Array Compaction

Instead of compacting the instruction array when eliminating dead code/stores:
- **Old approach**: Shift remaining instructions down, update all jump targets
- **New approach**: Mark dead instructions as `TCCIR_OP_NOP`, skip during codegen

Benefits:
1. **Simpler code** - no index remapping, no jump target patching
2. **Pool-friendly** - SValue pool indices remain valid
3. **Only peak heap matters** - slight extra memory for NOPs is acceptable
4. **Codegen already skips NOPs** - no additional changes needed

Implementation note during parallel-storage phase:
- When marking an instruction dead, set **both** `ir->instructions[i].op` and `ir->compact_instructions[i].op` to `TCCIR_OP_NOP`.

## Porting Strategy

### Recommended Order

1. **Start with optimization passes** (safest)
   - Run BEFORE codegen, so no peephole complications
   - Each pass can be migrated independently and tested
   - Start with simpler ones like `tcc_ir_dead_code_elimination()`

2. **Migration pattern for each function:**
   ```c
   // Old:
   TACQuadruple *q = &ir->instructions[i];
   q->field = value;

   // New:
   TACQuadruple q;
   tcc_ir_expand_quad(ir, i, &q);
   q.field = value;
   tcc_ir_writeback_quad(ir, i, &q);  // only if modified
   ```

3. **Leave `tcc_ir_generate_code()` for last**
   - Most complex due to peephole optimizations reading adjacent instructions
   - Peepholes (`ir_prev`/`ir_next`) need in-place modifications from prior iterations
   - Alternative: keep peepholes reading from `instructions[]` but main loop uses pool

4. **Backend functions (arm-thumb-gen.c) don't need migration**
   - They receive `TACQuadruple *q` as parameter from codegen
   - They don't access `ir->instructions[]` directly

### Memory Savings Goal

Once fully migrated:
- Remove `TACQuadruple *instructions` array (~160 bytes × N instructions)
- Keep only `IRQuadCompact *compact_instructions` (~16 bytes × N) + SValue pool
- Estimated ~10x memory reduction for instruction storage

## Remaining Work

### Phase 5: Full Migration (when ready for memory savings)

**Current Status**: 90 places in tccir.c still read from `ir->instructions[]`.
Optimization passes call `writeback_quad()` after modifications, but still READ
from the old array. Full migration requires switching reads to use
`expand_quad()` or access `compact_instructions` directly.

Migration approach:
1. Switch all READS to use expand/compact
2. Keep writing to BOTH arrays during transition (for any code still reading old)
3. Once all reads are migrated, remove the old array (Phase 6)

Note: `tcc_ir_generate_code()` peepholes read adjacent instructions and need
in-place modifications from prior iterations. These must keep reading from
`instructions[]` OR we must writeback immediately after each modification.

- [ ] Migrate reads in optimization passes (~60 sites)
  - [ ] `tcc_ir_dead_code_elimination()`
  - [ ] `tcc_ir_dead_store_elimination()`
  - [ ] `tcc_ir_constant_propagation()`
  - [ ] `tcc_ir_tmp_constant_propagation()`
  - [ ] `tcc_ir_copy_propagation()`
  - [ ] `tcc_ir_arithmetic_cse()`
  - [ ] `tcc_ir_bool_cse()`
  - [ ] `tcc_ir_bool_idempotent()`
  - [ ] `tcc_ir_bool_simplification()`
  - [ ] `tcc_ir_return_value_optimization()`
  - [ ] `tcc_ir_store_load_forwarding()`
  - [ ] `tcc_ir_redundant_store_elimination()`
  - [ ] `tcc_ir_liveness_analysis()`

- [ ] Migrate reads in other tccir.c functions (~30 sites)
  - [ ] `tcc_ir_put()` - coalescing logic reads prev instruction
  - [ ] `tcc_ir_get_vreg_definition_site()`
  - [ ] `tcc_ir_is_vreg_used_in_instruction()`
  - [ ] `tcc_ir_find_call_args()` and related
  - [ ] `tcc_ir_assign_registers()`
  - [ ] `tcc_ir_backpatch_jumps()`
  - [ ] Debug/print functions

- [ ] Migrate `tcc_ir_generate_code()` (~10 sites)
  - Main loop: `q = &ir->instructions[i]`
  - Peepholes: `ir_prev`, `ir_next` reads
  - Pre-loop: `max_orig_index`, `has_incoming_jump`

- [x] Backend in arm-thumb-gen.c - NO MIGRATION NEEDED
  - Backend functions receive `TACQuadruple *q` as parameter
  - They don't access `ir->instructions[]` directly

### Phase 6: Cleanup

- [ ] Remove `TACQuadruple *instructions` from TCCIRState
- [ ] Remove `instructions_size` field
- [ ] Rename `compact_instructions` to `instructions`
- [ ] Rename `compact_instructions_size` to `instructions_size`
- [ ] Update `tcc_ir_put()` to only store to pool
- [ ] Remove parallel storage code
- [ ] Keep `TACQuadruple` struct only for expand/writeback compatibility

## Optional Optimizations

- [ ] SValue deduplication in pool (for repeated constants)
- [ ] Pool compaction after DCE
- [ ] Direct accessor usage in hot paths (skip expand/writeback overhead)

## Testing

After each migration step:
```bash
make clean && make && make test -j32
```

All 466 tests should pass.
