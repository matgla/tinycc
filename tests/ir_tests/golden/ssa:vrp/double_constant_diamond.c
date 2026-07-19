/* ssa:vrp — nested always-true diamonds collapse.  This case was originally a
 * branch_fold_2x snapshot (legacy tcc_ir_opt_branch_folding_2x_ex: plain
 * branch_folding run twice back-to-back in the memory group).  That pass is
 * gone, and the work is now split across the pipeline: the group's sl_forward
 * trigger (the array store/load) plus const_cascade resolve both comparisons
 * to constant-vs-constant CMPs, kb_cascade drops the inner `a == 5` diamond,
 * and ssa:vrp — the last pass that still sees a CMP here — drops the outer
 * one, leaving only the `n + 1` return reachable. */
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
