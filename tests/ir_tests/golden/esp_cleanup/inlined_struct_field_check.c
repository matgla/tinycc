/* esp_cleanup: source/opt/engine/pipeline_table.c's
 * tcc_ir_opt_entry_store_cleanup_ex is a compound cleanup (const_prop_tmp/
 * const_var_prop/redundant_loop_check/dce/compact_nops/sl_forward/
 * dead_var_store_elim) run in the entry_store_prop group right after
 * entry_store forwards a field load. `check` is small enough to get inlined
 * into `main`, so its `x->j` access becomes a LOAD_INDEXED through a TEMP
 * copied from the inlined parameter -- entry_store's Phase 3b resolves it to
 * the entry-BB constant #5, and esp_cleanup const-folds the `x->j == 5` arm
 * into the `CMP #0,#0` diamond seen below.  The legacy branch_folding /
 * stack_addr_nonnull_fold members of this cleanup are gone, so collapsing the
 * now-constant diamonds down to `return 1` happens later, in the SSA passes;
 * the final code for `main` is still `movs r0, #1; bx lr`. */
struct A { int i; int j; };

static int check(struct A *x) {
    return x->i == 0 && x->j == 5;
}

int main(void) {
    struct A a = { .j = 5 };
    return check(&a);
}
