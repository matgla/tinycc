/* branch_cleanup: jump_thread + elim_fallthru + orphan_cmp cascade, looped to
 * a fixpoint in ir/opt_pipeline.c's late_cleanup group (which always runs,
 * unlike the memory group's jump_thread/elim_fallthru that are skipped when
 * sl_forward finds nothing to forward). After the pure `pure_call` is inlined
 * and its now-discarded result is DCE'd, the ternary's CMP+jump diamond is
 * dead code with no memory-forwarding trigger to reach it any other way. */
static int pure_call(int x);

int f(int a, int b) {
    a < b ? pure_call(a) : 0;
    return a + b;
}

static int pure_call(int x) { return x * 2; }

