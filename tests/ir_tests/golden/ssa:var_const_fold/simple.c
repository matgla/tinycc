/* Exercise ssa:var_const_fold: a non-promotable local VAR (address taken) is
   assigned a constant and then self-updated with an immediate in the same
   basic block.  The legacy memory_group pass leaves the self-update as
   `V0 <- V0 ADD #3`; ssa:var_const_fold should collapse it to `V0 <- #8`. */
int var_const_fold_simple(void) {
    int x = 5;
    x = x + 3;
    int *p = &x;
    *p = 7;
    return x;
}
