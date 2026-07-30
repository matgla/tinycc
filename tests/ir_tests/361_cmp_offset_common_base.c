/* Regression for the common-base branch of the CMP constant-offset fold:
 * A = X + K1, B = X + K2 with a shared base X reduces `A cond B` to
 * `K1 cond K2` under the signed-overflow-is-UB rule.  The base X is a runtime
 * param (from a volatile source), so the operands are genuine runtime reads —
 * the result is identical folded or not, so a miscompiled fold is what this
 * catches.  x is small, so no actual overflow occurs at runtime. */
#include <stdio.h>

volatile int vsrc = 1000;

int lt13(int x)   { int a = x + 1, b = x + 3; return a <  b; } /* 1 <  3 -> 1 */
int gt13(int x)   { int a = x + 1, b = x + 3; return a >  b; } /* 1 >  3 -> 0 */
int le31(int x)   { int a = x + 3, b = x + 1; return a <= b; } /* 3 <= 1 -> 0 */
int ne29(int x)   { int a = x + 2, b = x + 9; return a != b; } /* 2 != 9 -> 1 */
int ssub(int x)   { int a = x - 1, b = x - 4; return a >  b; } /* -1 > -4 -> 1 */
int negoff(int x) { int a = x - 5, b = x + 5; return a <  b; } /* -5 <  5 -> 1 */

int main(void)
{
    int x = vsrc;
    printf("%d\n", lt13(x));
    printf("%d\n", gt13(x));
    printf("%d\n", le31(x));
    printf("%d\n", ne29(x));
    printf("%d\n", ssub(x));
    printf("%d\n", negoff(x));
    return 0;
}
