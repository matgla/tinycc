/* Regression test (reduced differential-fuzz repro, gen_c.py seed=2049).
 * ra_fold_const_branches (ir/regalloc.c) folds a JUMPIF whose CMP has two
 * constant operands by NOPing that CMP.  Walking back from the JUMPIF to find
 * its flag-setting CMP, it strode past a soft-float compare *call*
 * (__aeabi_cfcmple, a FUNCCALLVOID flag-setter emitted by the `f16 < .. ||
 * f16 > ..` clamp) — which is the branch's real flag source — and mis-attributed
 * the branch to an earlier *integer* CMP that actually feeds a SELECT.  It then
 * NOPed that integer CMP, leaving the SELECT (an ITE block) reading stale flags,
 * so `r = -(u9 <= ..)` came out wrong.  No -fno-* knob gated it (backend pass).
 * Fix: stop the walk-back at calls (CPSR is caller-clobbered) and at flag
 * consumers (SETIF/SELECT).  tcc -O0 correct; -O1/-O2 diverged.
 * Expected checksum is gcc -m32 -funsigned-char (ABI-independent here).
 */
#include <stdio.h>
#include <string.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

static unsigned fbits_f(float f)
{
  unsigned u;
  memcpy(&u, &f, sizeof u);
  return u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u9 = 2504194783u;
  float f16 = -0x1.6802300000000p+16f;
  /* Integer comparison feeding a SELECT (the conditional `-(u9 <= K)`). */
  unsigned b = (u9 <= (756551856u ^ cs)) ? 1u : 0u;
  unsigned r = (unsigned)(-b);
  /* Float clamp: lowers to __aeabi_cfcmple calls + a JUMPIF — the trigger. */
  f16 = (f16 < -0x1p40f || f16 > 0x1p40f) ? (float)1 : f16;
  cs = csmix(cs, r);
  cs = csmix(cs, fbits_f(f16));
  printf("checksum=%08x\n", cs);
  return 0;
}
