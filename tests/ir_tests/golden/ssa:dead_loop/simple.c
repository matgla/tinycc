int simple(void) {
    int o = 0;
    int i;
    /* Trip 100: above both LCS_MAX_TRIP_COUNT and UNROLL_MAX_TRIP_COUNT, so the
       loop reaches ssa:dead_loop instead of being folded or unrolled away
       upstream -- this case is here to exercise dead_loop itself. */
    for (i = 0; i < 100; i++) {
        o = 42;
    }
    return o;
}
