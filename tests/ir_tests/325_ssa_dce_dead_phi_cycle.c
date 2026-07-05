/* Exercise ssa_opt_dce.c: dead phi-cycle elimination.
 * The 'dead' loop-carried value feeds only itself and should be removed,
 * while the live loop-carried values s and i must be preserved. */
#include <stdio.h>

static unsigned f(unsigned n)
{
    unsigned i = 0, s = 0, dead = 0;
    while (i < n) {
        dead = dead + 1;
        s = s + i;
        i = i + 1;
    }
    return s;
}

int main(void)
{
    printf("%u\n", f(10));
    return f(10) != 45;
}
