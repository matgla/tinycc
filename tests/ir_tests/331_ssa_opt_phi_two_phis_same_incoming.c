/* Exercise ssa_opt.c: phi-operand / use-def bookkeeping.
 * A single incoming value from one predecessor feeds two phis at the same
 * merge point; replacement of that value must update both phi operands. */
#include <stdio.h>

static int g(int x)
{
    return x + 7;
}

static int f(int cond)
{
    int a, b, t;
    if (cond) {
        t = g(5);
        a = t;
        b = t;
    } else {
        t = 0;
        a = 1;
        b = 2;
    }
    return a + b;
}

int main(void)
{
    printf("%d %d\n", f(1), f(0));
    return 0;
}
