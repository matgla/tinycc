/* Exercise ssa_opt_cprop.c: forward single-def local variables into
 * later uses (cross-block / dominated).  The constant 42 assigned to x
 * must reach the accumulating loop. */
#include <stdio.h>

static int f(int n)
{
    int x = 42;
    int y = x;
    for (int i = 0; i < n; i++)
        y += i;
    return y;
}

int main(void)
{
    printf("%d\n", f(10));
    return 0;
}
