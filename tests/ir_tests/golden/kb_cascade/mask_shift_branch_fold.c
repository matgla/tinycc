/* kb_cascade: ir/opt_pipeline.c's tcc_ir_opt_known_bits_cascade_ex loops
 * known_bits -> const_prop_tmp -> branch_folding -> dce -> elim_fallthrough ->
 * sl_forward to a fixpoint within one pipeline step. `5 & 0xF0` has
 * statically-known-zero low bits, so known_bits resolves `(5 & 0xF0) >> 4` to
 * 0, which folds the `== 0` branch, DCEs the dead arm, and (via the re-run
 * sl_forward inside the cascade) collapses the whole function to a single
 * constant return -- more than one constituent pass must fire in sequence for
 * this to converge in one pipeline invocation. */
int f(int cond) {
    int arr[4];
    arr[0] = 5;
    int v = arr[0];
    int w = v & 0xF0;
    int z = w >> 4;
    if (z == 0) {
        return z + 100;
    }
    return z;
}
