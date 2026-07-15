/* __aeabi_lmod / __aeabi_ulmod are now classified pure (tcc_ir_is_pure_aeabi),
 * so a loop-invariant modulo can be hoisted and identical modulo expressions can
 * be CSE'd.  Operands are volatile-sourced (runtime, not constant) so the modulo
 * is NOT const-folded away — this exercises the pure-call consumers (LICM / CSE /
 * DCE) on the modulo helpers, which a miscompile of the 64-bit return would break.
 * -O0 keeps every call; -O1/-O2/-Os may transform them.  All must agree.
 */
#include <stdio.h>

static long long g(long long x, long long y)
{
  long long acc = 0;
  for (int i = 0; i < 4; i++)
    acc += x % y;             /* loop-invariant modulo -> hoist candidate */
  acc += (x % y) - (x % y);   /* identical calls -> CSE candidate, nets 0  */
  return acc;
}

static unsigned long long ug(unsigned long long x, unsigned long long y)
{
  unsigned long long acc = 0;
  for (int i = 0; i < 4; i++)
    acc += x % y;
  return acc;
}

int main(void)
{
  volatile long long vx = -1000000000007LL, vy = 13;
  volatile unsigned long long uvx = 18446744073709551615ULL, uvy = 13;
  long long r = g(vx, vy);
  unsigned long long ur = ug(uvx, uvy);
  printf("r=%lld ur=%llu\n", r, ur);
  return 0;
}
