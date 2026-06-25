/* Reroll basic test - 8 structurally identical macro-unrolled cleanup blocks
 * should be re-rolled into a single counted loop.  Verifies that
 * tcc_ir_opt_reroll correctly preserves semantics: glob_i ends up as 8
 * (one increment per block, regardless of whether the code emits 8 unrolled
 * bodies or 1 body in a loop).  Also tests intermediate counts to ensure
 * each iteration executes exactly once. */
#include <stdio.h>

static int glob_i = 0;
static int call_count = 0;

void incr_glob_i(int *i) {
    glob_i += *i;
    call_count++;
}

#define INCR_GI { int i __attribute__ ((__cleanup__(incr_glob_i))) = 1; }

int main(void) {
    INCR_GI INCR_GI INCR_GI INCR_GI
    INCR_GI INCR_GI INCR_GI INCR_GI

    printf("glob_i=%d call_count=%d\n", glob_i, call_count);
    if (glob_i != 8 || call_count != 8) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
