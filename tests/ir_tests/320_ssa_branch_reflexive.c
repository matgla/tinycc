/* Exercise ssa_opt_branch.c: CMP x,x reflexive comparison fold.
 * After copy propagation the compare may see two TEMPs that chase-copies
 * resolve to the same vreg; the result must still be "equal". */
#include <stdio.h>

static int f(int x)
{
    int y = x;
    int z = y;
    return (z == y) ? 11 : 22;
}

int main(void)
{
    printf("%d %d\n", f(5), f(0));
    return 0;
}
