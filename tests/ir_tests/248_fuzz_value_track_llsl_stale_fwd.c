/* Regression: value_tracking lowered an __aeabi_llsl (64-bit shift-left) call
 * with a known shift amount to a native SHL IR instruction, but did not
 * invalidate its value-tracking state for the destination VAR, so a later
 * read of that VAR was forwarded its STALE pre-shift constant.
 *
 * From longlong fuzz seed 2057 (reduced).  tcc -O0 agreed with gcc; only
 * tcc -O1/-O2 diverged — an optimizer miscompile.
 *
 * Root cause: q13 is first initialized to a compile-time constant
 * `(u10<<32)|u11`, then reassigned via `helper3(...) % k) << shiftamt` where
 * shiftamt happens to fold to the compile-time constant 32 but the shifted
 * value does not.  tcc_ir_opt_value_tracking's "lower shift calls with
 * immediate shift amount to IR instructions" path (ir/opt_constprop.c,
 * near the __aeabi_llsl/llsr/lasr fold) rewrites the CALL into `V4 <- T12
 * SHL #32` in place but forgot to invalidate q13's tracked constant state
 * (unlike the sibling __aeabi_uldivmod fold, which already does this — see
 * test 243).  The value-tracking map therefore still held q13's original
 * init constant, and forwarded it into the following
 * `(unsigned)q13 ^ (unsigned)(q13>>32)` instead of the reassigned value.
 *
 * Fix: invalidate the dest VAR in the value-tracking map right after
 * lowering the shift call to an IR instruction, mirroring the uldivmod fix.
 */
#include <stdio.h>

static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(3885229385u) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  char s6 = (char)(1435334846u & 0xff);
  unsigned u10 = 901479600u;
  unsigned u11 = 878673281u;
  unsigned long long q13 = (((unsigned long long)(u10)) << 32) | (unsigned long long)(u11);
  struct S st15 = { 1902471566u, 1018451216u, 3599505179u };

  q13 = (((unsigned long long)(unsigned)(((unsigned)(helper3(st15.f1, u10)) % ((unsigned)(((unsigned)(292412237u) - (unsigned)(2389354907u))) | 1u))))) << ((unsigned)(((unsigned)(st15.f1) * (unsigned)((unsigned)(s6)))) & 63u);

  cs = cs ^ ((unsigned)(q13) ^ (unsigned)(q13 >> 32));  /* must see reassigned q13 */
  printf("checksum=%08x\n", cs);
  return 0;
}
