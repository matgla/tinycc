/* const_cascade: ir/opt_pipeline.c's tcc_ir_opt_const_prop_cascade_ex loops
 * const_prop/const_prop_tmp/const_var_prop/value_tracking to a fixpoint
 * within the memory group. The stack-stored constant is only revealed by
 * sl_forward (this group's trigger, run first); the propagation group's own
 * const_prop already ran and converged *before* that value was visible, so
 * folding the whole (5+3)*2 chain to a single constant requires this second,
 * post-forwarding cascade rather than the earlier one-shot const_prop. */
int f(int cond) {
    int arr[4];
    arr[0] = 5;
    int v = arr[0];
    int w = v + 3;
    int z = w * 2;
    if (!cond) goto skip;
    return z;
skip:
    return 0;
}
