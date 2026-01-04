## Plan: Phase 4 - Alias Analysis and Store/Load Optimizations

Implement store-load forwarding and redundant load/store elimination using a simple intra-procedural alias analysis to enable safe memory operation optimizations in the TinyCC IR.

### Steps

1. **Implement Memory Location Tracking in [tccir.c](tccir.c)** - Add data structures (`MemoryLocation`, `StoreEntry` hash table) to track recent store addresses and values, distinguishing stack locals (`VT_LOCAL`) from pointer-based accesses.

2. **Implement `tcc_ir_store_load_forwarding()` in [tccir.c](tccir.c)** - For `TCCIR_OP_LOAD` instructions, check if the address matches a tracked store entry; if so, replace the LOAD with an ASSIGN from the stored value.

3. **Implement Alias Analysis Invalidation Logic** - Invalidate store entries conservatively: clear all pointer-based entries on any unknown pointer store; clear all entries at basic block boundaries (`JUMP`, `JUMPIF`) and function calls (`FUNCCALLVOID`, `FUNCCALLVAL`); preserve provably non-aliasing stack locals.

4. **Implement `tcc_ir_redundant_store_elimination()` in [tccir.c](tccir.c)** - Remove stores to addresses that are overwritten before being read (dead stores to memory), using the same alias tracking infrastructure.

5. **Integrate Phase 4 into Optimization Pipeline in [tccgen.c](tccgen.c#L10027)** - Add `tcc_ir_store_load_forwarding()` and `tcc_ir_redundant_store_elimination()` after Phase 3 CSE, before final dead store elimination.

6. **Update [ir_optimization_plan.md](docs/ir_optimization_plan.md)** - Document Phase 4 implementation details, alias analysis rules, expected results for the `Move()` example, and mark as complete.

### Further Considerations

1. **Alias Precision Level?** Start with a conservative type-based approach (stack locals never alias pointer derefs) or implement offset-based tracking for array accesses? *Recommend: Start conservative, extend later.*

2. **Struct/Array Member Handling?** Track field offsets for structs (e.g., `dest[j-1]` vs `dest[j]` are different addresses)? *Recommend: Track base+offset pairs for indexed accesses.*

3. **Cross-Basic-Block Optimization?** Keep analysis local to basic blocks initially, or implement reaching-stores dataflow? *Recommend: Start with basic-block-local, simpler and safer.*
