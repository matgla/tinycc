/* Exercise ssa_opt_dead_loop.c: side-effect-free loop with a runtime bound.
 * When the trip count is not provably >= 1, the pass emits a guarded
 * SELECT between the preheader value and the latch constant. */
#include <stdio.h>

static unsigned f(unsigned n)
{
    unsigned x = 1;
    for (unsigned i = 0; i < n; i++)
        x = 0xDEADBEEFu;
    return x;
}

int main(void)
{
    printf("%08x %08x\n", f(0), f(4));
    return 0;
}
