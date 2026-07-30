/* Signed division/remainder by a power-of-two constant must follow C
 * semantics: division rounds toward zero and the remainder takes the sign of
 * the dividend.  A naive `x >> n` / `x & (2^n-1)` is WRONG for negative x
 * (ASR rounds toward -inf, AND is always non-negative).  These checks pin the
 * negative cases regardless of how the backend lowers it (hardware SDIV here).
 * The dividend is volatile so the value is computed at runtime.
 */
#include <stdio.h>

volatile int V;

int main(void)
{
  int ok = 1;

#define CK_DIV(x, d, exp) do { V = (x); if (V / (d) != (exp)) ok = 0; } while (0)
#define CK_MOD(x, d, exp) do { V = (x); if (V % (d) != (exp)) ok = 0; } while (0)

  /* division rounds toward zero */
  CK_DIV(-1, 2, 0);            /* naive ASR would give -1 */
  CK_DIV(1, 2, 0);
  CK_DIV(-7, 4, -1);
  CK_DIV(-8, 4, -2);
  CK_DIV(-9, 4, -2);
  CK_DIV(7, 4, 1);
  CK_DIV(8, 4, 2);
  CK_DIV(-1024, 256, -4);
  CK_DIV(123, 1024, 0);
  CK_DIV(-2147483647 - 1, 2, -1073741824);   /* INT_MIN / 2 */

  /* remainder takes the sign of the dividend */
  CK_MOD(-3, 4, -3);          /* naive AND would give 1 */
  CK_MOD(-5, 4, -1);
  CK_MOD(-8, 4, 0);
  CK_MOD(7, 4, 3);
  CK_MOD(8, 4, 0);
  CK_MOD(-1, 2, -1);
  CK_MOD(1023, 256, 255);
  CK_MOD(-1000, 8, 0);
  CK_MOD(-1001, 8, -1);
  CK_MOD(-2147483647 - 1, 2, 0);              /* INT_MIN % 2 */

  printf("%s\n", ok ? "OK" : "FAIL");
  return 0;
}
