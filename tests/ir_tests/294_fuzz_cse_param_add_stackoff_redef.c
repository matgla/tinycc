/*
 * int fuzz seed 41379 reduction (O1/O2 wrong-code):
 * the narrow ADD/SUB CSE `tcc_ir_opt_cse_param_add` (ir/opt_copyprop.c)
 * deduplicates `X +/- #imm` expressions within a basic block.  A stack
 * local read as an lvalue is keyed by a synthetic STACKOFF key
 * (0x70000000|pos), but a plain register-form write to the same local
 * (`u4 = <compare result>`) has a raw VAR vreg and only invalidated raw-key
 * entries -- never the synthetic STACKOFF key.  So the two `u4 - 27586`
 * computations here (the first inside the value assigned to u4, reading the
 * OLD u4; the second in `pa` after the reassignment, reading the NEW u4)
 * were wrongly CSE'd together across the redefinition, and `pa` used the
 * stale pre-assignment value of u4.  Fixed by having a register-form write
 * invalidate BOTH the raw and the STACKOFF synthetic key for the same local.
 *
 * helper1's two args must both be non-trivial expressions of u4 so the
 * inlined body keeps the mis-CSE'd `pa` live; with pa folded to a literal the
 * divergence disappears.
 *
 * Ground truth (tcc -O0 == tcc -O1/-O2/-Os == arm-none-eabi-gcc -O2): 00006a8a.
 */
#include <stdio.h>

static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(pb) ^ lr;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  short s3 = (short)(133852098u & 0xffff);   /* = 27586 */
  unsigned u4 = 4284604924u;
  unsigned u5 = 2213543727u;
  unsigned u6 = 3954978203u;

  if ((unsigned)(u5) & 1u) {
      /* reassign u4 from a compare; the `u4 - s3` inside reads the OLD u4 */
      u4 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) / ((unsigned)((unsigned)(s3)) | 1u))) << ((unsigned)((unsigned)(s3)) & 31u))) % ((unsigned)(((unsigned)(((unsigned)(u6) + (unsigned)(((unsigned)(u6) ^ cs)))) | (unsigned)(u5))) | 1u))) <= ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) - (unsigned)((unsigned)(s3)))) ^ (unsigned)((-((unsigned)((unsigned)(s3)) | 0u))))) & (unsigned)(((unsigned)(((unsigned)(472158516u) & (unsigned)(u4))) ^ (unsigned)(u4))))) ^ cs))) & 0xffffffffu;
      /* pa's `u4 - s3` must read the NEW (reassigned) u4 */
      unsigned pa = (~((unsigned)(((unsigned)(u4) - (unsigned)((unsigned)(s3)))) | 0u));
      unsigned pb = ((unsigned)(u4) | (unsigned)(((unsigned)(3135934056u) >> ((unsigned)(1929147097u) & 31u))));
      cs = helper1(pa, pb);
  }

  printf("checksum=%08x\n", cs);
  return 0;
}
