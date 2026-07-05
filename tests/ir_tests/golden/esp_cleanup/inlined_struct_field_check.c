/* esp_cleanup: ir/opt_pipeline.c's tcc_ir_opt_entry_store_cleanup_ex is a
 * compound cleanup (const_prop/const_prop_tmp/const_var_prop/branch_folding/
 * stack_addr_nonnull_fold/redundant_loop_check/dce/compact_nops/sl_forward/
 * dead_var_store_elim) run in the entry_store_prop group right after
 * entry_store forwards a field load. `check` is small enough to get inlined
 * into `main`, so its `x->j` access becomes a LOAD_INDEXED through a TEMP
 * copied from the inlined parameter -- entry_store's Phase 3b resolves it to
 * the entry-BB constant #5, and esp_cleanup then collapses the whole
 * now-constant `x->i == 0 && x->j == 5` check down to a single `return 1`. */
struct A { int i; int j; };

static int check(struct A *x) {
    return x->i == 0 && x->j == 5;
}

int main(void) {
    struct A a = { .j = 5 };
    return check(&a);
}
