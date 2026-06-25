/* Reroll negative test - blocks that look superficially similar but
 * use distinct constants (1, 2, 3, ...) per iteration.  Reroll MUST NOT
 * fire here: the per-iteration immediates differ in src1, so the
 * structural fingerprint mismatches and rerolling would change
 * semantics. */
#include <stdio.h>

static int sum = 0;

void add_to_sum(int *v) { sum += *v; }

#define ADD(N) { int x __attribute__ ((__cleanup__(add_to_sum))) = (N); }

int main(void) {
    ADD(1) ADD(2) ADD(3) ADD(4)
    ADD(5) ADD(6) ADD(7) ADD(8)

    /* 1+2+...+8 = 36.  If the pass had incorrectly rolled these into
     * a loop using only the first body, we'd get 1*8 = 8. */
    printf("sum=%d\n", sum);
    if (sum != 36) {
        printf("FAIL expected 36\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
