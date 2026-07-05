/* Exercise ssa_opt_fold.c: 64-bit constant folding.
 * A 64-bit UDIV of two I64 constants must be evaluated at compile time. */
#include <stdio.h>

static unsigned long long f(void)
{
    unsigned long long a = 0x123456789ABCDEF0ULL;
    unsigned long long b = 0x10ULL;
    return a / b;
}

int main(void)
{
    printf("%016llx\n", (unsigned long long)f());
    return 0;
}
