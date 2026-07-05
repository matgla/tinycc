/* Exercise ssa_opt_cmp_eq.c: equality/inequality fact propagation
 * through the dominator tree.  Facts pushed by an outer CMP+JEQ must be
 * visible in dominated blocks and correctly popped when leaving the subtree. */
#include <stdio.h>

static int f(int a, int b, int c, int d)
{
    if (a == b) {
        if (c == d) {
            if (a == b) return 1;
            if (c != d) return 2;
        }
        if (a != b) return 3;
    }
    return 0;
}

int main(void)
{
    printf("%d %d %d %d\n", f(1, 1, 2, 2), f(1, 1, 2, 3), f(1, 2, 2, 2), f(1, 2, 3, 3));
    return 0;
}
