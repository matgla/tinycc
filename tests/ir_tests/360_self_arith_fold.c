/* Regression for the self-arithmetic identity fold (self_arith): x/x -> 1,
 * x%x -> 0.  Values come from a volatile source so the operands are runtime
 * register/global reads (exercising self_arith, not immediate const-folding).
 * Correctness is identical with or without the fold, so a miscompiled fold
 * (e.g. x%x -> 1) is what this catches. */
#include <stdio.h>

volatile int vsrc = 7;
int g;

/* Same-register self-divide (new vreg path): param read twice. */
int sdiv(int x) { return x / x; }
int smod(int x) { return x % x; }
unsigned udiv(unsigned x) { return x / x; }
unsigned umod(unsigned x) { return x % x; }
long long lldiv_(long long x) { return x / x; }

/* Same-global self-divide (original symref path). */
int gdiv(void) { return g / g; }
int gmod(void) { return g % g; }

int main(void)
{
    int a = vsrc;          /* runtime 7 */
    printf("%d\n", sdiv(a));            /* 1 */
    printf("%d\n", smod(a));            /* 0 */
    printf("%u\n", udiv((unsigned)a));  /* 1 */
    printf("%u\n", umod((unsigned)a));  /* 0 */
    printf("%lld\n", lldiv_((long long)a)); /* 1 */

    g = vsrc;              /* runtime 7 */
    printf("%d\n", gdiv());             /* 1 */
    printf("%d\n", gmod());             /* 0 */

    /* Negative operand: x/x is still 1, x%x still 0. */
    int b = -vsrc;         /* runtime -7 */
    printf("%d\n", sdiv(b));            /* 1 */
    printf("%d\n", smod(b));            /* 0 */
    return 0;
}
