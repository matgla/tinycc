/* Phase 4: atomic load/store/exclusive mappings. */

int atomic_load(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
void atomic_store(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
int atomic_add(int *p, int v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
int atomic_cmpxchg(int *p, int e, int d) { return __atomic_compare_exchange_n(p, &e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
