/* Exercise ssa_opt_branch.c: unsigned 32-bit constant comparison folding.
 * A value with the high bit set (0x80000000U) must compare as greater than
 * 0x7fffffffU in unsigned arithmetic. */
#include <stdio.h>

static int f(unsigned a, unsigned b)
{
    return (a < b) ? 1 : 0;
}

int main(void)
{
    printf("%d %d %d\n", f(0x80000000U, 0x7fffffffU), f(1U, 2U), f(5U, 5U));
    return 0;
}
