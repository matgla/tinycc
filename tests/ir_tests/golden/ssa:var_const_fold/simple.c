/* Guards the address-taken bail-out of ssa:var_const_fold: a non-promotable
   local VAR is assigned a constant and then self-updated with an immediate, but
   its address has escaped to a call that sits BETWEEN the two defs.  Collapsing
   `V0 <- #5` / `V0 <- V0 ADD #3` into `V0 <- #8` would therefore be a
   miscompile, not merely an over-eager fold -- sink() may have written x.  The
   firing shape (no address taken) is covered by
   tests/unit/arm/armv8m/test_ssa_opt_cprop.c.

   The escape has to be a call: the earlier version stored through the pointer
   locally (`int *p = &x; *p = 7; return x;`) and later passes learned to fold
   that whole body to `return 7` before ssa:var_const_fold ever ran, so the case
   stopped exercising the bail-out it was written to guard. */
void sink(int *);
int var_const_fold_simple(void) {
    int x = 5;
    int *p = &x;
    sink(p);
    x = x + 3;
    return x;
}
