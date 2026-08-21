/* esp_cleanup: source/opt/engine/pipeline_table.c's
 * tcc_ir_opt_entry_store_cleanup_ex is a compound cleanup (const_prop_tmp/
 * const_var_prop/redundant_loop_check/dce/compact_nops/sl_forward/
 * dead_var_store_elim) run in the entry_store_prop group right after
 * entry_store forwards a field load.  `add_fields` is small enough to get
 * inlined into `main`, so its `x->i` / `x->j` accesses become a
 * `T***DEREF***` and a LOAD_INDEXED through TEMPs copied from the inlined
 * parameter.  entry_store's Phase 3b resolves the indexed one to the entry-BB
 * constant #5, and esp_cleanup's const_var_prop then resolves the remaining
 * deref to #3, leaving the `#3 ADD #5` seen below for later passes to fold.
 *
 * The result is handed to an EXTERNAL function on purpose.  esp_cleanup only
 * runs when `entry_store`, the group's trigger, reports a change, and an idle
 * trigger ends the group before its other members run (pipeline_run.c).  With
 * a foldable consumer -- the original `return x->i == 0 && x->j == 5;` -- the
 * earlier propagation group collapses the whole diamond to `return 1` and
 * entry_store arrives with nothing left to forward, so the pass under test
 * never executes.  An opaque sink keeps the struct reads alive that far. */
struct A { int i; int j; };

extern int sink(int);

static int add_fields(struct A *x) {
    return x->i + x->j;
}

int main(void) {
    struct A a = { .i = 3, .j = 5 };
    return sink(add_fields(&a));
}
