/* kb_cascade: ir/opt_pipeline.c's tcc_ir_opt_known_bits_cascade_ex loops
 * known_bits -> const_prop_tmp -> branch_folding -> dce -> elim_fallthrough ->
 * sl_forward to a fixpoint within one pipeline step. `5 & 0xF0` has
 * statically-known-zero low bits, so known_bits resolves `(5 & 0xF0) >> 4` to
 * 0, which folds the `== 0` branch, DCEs the dead arm, and (via the re-run
 * sl_forward inside the cascade) collapses the whole function to a single
 * constant return -- more than one constituent pass must fire in sequence for
 * this to converge in one pipeline invocation.
 *
 * The runtime-indexed store keeps entry_store_prop out of it: that pass would
 * otherwise forward the constant itself, leaving this group's sl_forward trigger
 * idle and the cascade never running -- i.e. the case would stop covering it.
 *
 * What the snapshot actually pins today is weaker than the paragraph above:
 * `v`/`w`/`z` land in VARs, which the known_bits lattice does not track (it
 * follows TMPs and stack slots), so by the time kb_cascade runs, const_cascade
 * has already folded the chain and setif_fuse has already folded the guard --
 * this slot only compacts the NOPs they left. The three dead `V0/V1/V2 <-- #k`
 * assigns survive to here because const_var_prop's dominance guard (see
 * source/opt/flat/scalar/const_var_prop.c) drops a VAR's constant when a use
 * sits at a jump target, and its phase 3 only NOPs defs it still believes are
 * constant; the late dead-store passes remove them, and the function does end
 * up as a single `RETURNVALUE #100`. Restoring true kb_cascade coverage needs a
 * case whose known bits live in a TMP or stack slot and are revealed only by
 * the memory group's sl_forward. */
int f(int cond, int n) {
    int arr[4];
    arr[n & 3] = 1;
    arr[0] = 5;
    int v = arr[0];
    int w = v & 0xF0;
    int z = w >> 4;
    if (z == 0) {
        return z + 100;
    }
    return z;
}
