/* const_cascade: ir/opt_pipeline.c's tcc_ir_opt_const_prop_cascade_ex loops
 * const_prop/const_prop_tmp/const_var_prop/value_tracking to a fixpoint
 * within the memory group. The stack-stored constant is only revealed by
 * sl_forward (this group's trigger, run first); the propagation group's own
 * const_prop already ran and converged *before* that value was visible, so
 * folding the whole (5+3)*2 chain to a single constant requires this second,
 * post-forwarding cascade rather than the earlier one-shot const_prop.
 *
 * The runtime-indexed store keeps entry_store_prop out of it: that pass would
 * otherwise forward the constant itself, leaving this group's sl_forward trigger
 * idle and the cascade never running -- i.e. the case would stop covering it.
 *
 * The snapshot stops at `V2 <-- #16` and still loads V2 for the return: `z`'s
 * only use sits at a jump target (the `goto skip` diamond), and const_var_prop's
 * dominance guard refuses to forward a constant VAR into a use it cannot cheaply
 * prove the def dominates (see the guard's comment in
 * source/opt/flat/scalar/const_var_prop.c -- it is what fixes the `||` chain
 * whose two arms write different vregs). The cascade's own work is still what
 * this snapshot pins: on entry the chain is `V1 <-- V0 ADD #3` / `V2 <-- V1 SHL
 * #1`, and the SSA pipeline turns the tail into `RETURNVALUE #16` later on. */
int f(int cond, int n) {
    int arr[4];
    arr[n & 3] = 1;
    arr[0] = 5;
    int v = arr[0];
    int w = v + 3;
    int z = w * 2;
    if (!cond) goto skip;
    return z;
skip:
    return 0;
}
