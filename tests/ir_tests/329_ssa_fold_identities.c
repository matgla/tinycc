/* Exercise ssa_opt_fold.c: algebraic identity folding.
 * x+0, x|0, x^0, x*1 and x&0xFFFFFFFF must all collapse to x. */
#include <stdio.h>

static unsigned f(unsigned x)
{
    unsigned a = x + 0;
    unsigned b = a | 0;
    unsigned c = b ^ 0;
    unsigned d = c * 1;
    unsigned e = d & 0xFFFFFFFFu;
    return e;
}

int main(void)
{
    printf("%08x\n", f(0x12345678));
    return 0;
}
