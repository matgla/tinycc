/* Exercise ssa_opt_cprop.c: ssa:var_const_fold peephole.
 * Folding a self-update x = x OP #imm must not delete the prior constant
 * def when there is an intervening use of that def. */
#include <stdio.h>

int main(void)
{
    int x = 5;
    int y = x + 1;
    x = x + 2;
    printf("%d %d\n", y, x);
    return (y == 6 && x == 7) ? 0 : 1;
}
