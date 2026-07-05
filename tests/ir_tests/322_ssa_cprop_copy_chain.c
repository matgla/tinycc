/* Exercise ssa_opt_cprop.c: copy propagation through ASSIGN chains.
 * The final use of c should be rewritten to use the original x directly. */
#include <stdio.h>

static int f(int x)
{
    int a = x;
    int b = a;
    int c = b;
    return c + 1;
}

int main(void)
{
    printf("%d\n", f(7));
    return 0;
}
