/* Exercise ssa_opt_dead_loop.c: side-effect-free loop with a
 * compile-time-constant bound.  The loop body assigns a constant to x on
 * every iteration, so the final value is the latch constant. */
#include <stdio.h>

static unsigned f(void)
{
    unsigned x = 1;
    for (unsigned i = 0; i < 3; i++)
        x = 0xCAFEBABEu;
    return x;
}

int main(void)
{
    printf("%08x\n", f());
    return 0;
}
