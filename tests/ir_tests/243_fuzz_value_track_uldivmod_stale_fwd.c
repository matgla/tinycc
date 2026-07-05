/* Regression: value_tracking constant-folded a __aeabi_uldivmod call to
 * `V <- #quotient` but did not update its value-tracking state for V, so a later
 * read of V was forwarded V's STALE pre-division constant.
 *
 * From combo_num fuzz seed 58 (reduced).  tcc -O0/-O1/-Os agreed with gcc; only
 * tcc -O2 diverged — an O2 optimizer miscompile.
 *
 * Root cause: the post-loop-unroll cleanup (tccgen.c) re-runs const-prop + DCE +
 * value_tracking.  Once loop unrolling collapses the pre-loop code into one basic
 * block, value_tracking scans it forward with a running VAR-constant map.  It
 * const-folds `q10 = q10 / c` (a __aeabi_uldivmod call, both args constant) into
 * `q10 <- #quotient` and `continue`s — which skips the general VAR-def
 * invalidation at the loop tail.  The map therefore still held q10's PRE-division
 * init `(u5<<32)|u6` (from `q10 = ...`), and that stale value was forwarded into
 * the following `(unsigned)q10 ^ (unsigned)(q10>>32)`, computing `u6 ^ u5`
 * instead of the divided value.
 *
 * The float subtraction + `if (f17 < ...)` and the array loop are needed only to
 * make loop-unroll collapse the prefix into a single block so the buggy forward
 * fires; the essence is the folded 64-bit divide followed by a read of q10.
 *
 * Fix: after value_tracking folds the __aeabi_uldivmod/ldivmod call, invalidate
 * the dest VAR in the value-tracking map so the stale pre-call constant is not
 * forwarded (the rewritten `q10 <- #quotient` still carries the correct value).
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u5 = 558971978u;
  unsigned u6 = 3594152618u;
  unsigned arr7[2] = { 4081765584u, 648513156u };
  unsigned long long q10 = (((unsigned long long)(u5)) << 32) | (unsigned long long)(u6);
  double f17 = -0x1.9d73700000000p+18;
  float f18 = 0x1.dc64900000000p+28f;

  f17 = ((double)(f18)) - ((double)((unsigned)(u6)));
  if (f17 < -0x1p40) f17 = 1;
  q10 = (q10) / ((((unsigned long long)(unsigned)(3406872265u))) | 1ull);

  cs = csmix(cs, u5);
  cs = csmix(cs, (unsigned)(q10) ^ (unsigned)(q10 >> 32));  /* must see divided q10 */
  for (unsigned k = 0u; k < 2u; k++)
    cs = csmix(cs, arr7[k]);
  (void)f17;
  printf("checksum=%08x\n", cs);
  return 0;
}
