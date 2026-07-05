/* branch_fold_2x: ir/opt_pipeline.c's tcc_ir_opt_branch_folding_2x_ex is
 * literally tcc_ir_opt_branch_folding() called twice back-to-back in the
 * memory group. Needs the group's sl_forward trigger (the array store/load)
 * to even run; by the time const_cascade (which runs just before this pass
 * in the same group) resolves both comparisons to constant-vs-constant CMPs,
 * branch_folding folds both diamonds away within this single pipeline step. */
int f(int n) {
    int arr[4];
    arr[0] = 5;
    int a = arr[0];
    int b = a;
    int c = (a == b);
    if (c) {
        if (a == 5) {
            return n + 1;
        }
    }
    return n;
}
