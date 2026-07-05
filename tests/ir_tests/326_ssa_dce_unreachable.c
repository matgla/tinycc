/* Exercise ssa_opt_dce.c: unreachable-code elimination.
 * The call after the unconditional return must be NOP'd and must not
 * increment the global counter. */
#include <stdio.h>

static int count = 0;

static void bump(void)
{
    count++;
}

static int f(int x)
{
    return x;
    bump();
    return x + 1;
}

int main(void)
{
    printf("%d %d\n", f(5), count);
    return (f(5) == 5 && count == 0) ? 0 : 1;
}
