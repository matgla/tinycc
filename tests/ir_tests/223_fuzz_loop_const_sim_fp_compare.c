/* Regression test (reduced differential-fuzz repro, gen_c.py seed=544).
 * tcc_ir_opt_loop_const_sim (gated by -floop-unroll) constant-simulates a
 * fixed-trip loop at compile time.  A float comparison in the body lowers to a
 * cfcmple/cdcmple flag-setter that stores raw FP *bit patterns* in cmp_v1/
 * cmp_v2; the JUMPIF handler then evaluated them with evaluate_compare_condition
 * (integer compare), ignoring the cmp_is_fp flag.  A negative float's bit
 * pattern reads as a huge unsigned int, so `f9 <= (float)i` (f9 < 0, always
 * true) folded to the wrong branch and the loop collapsed to a wrong constant.
 * Only the O2 result diverged (loop_const_sim/unroll are O2-only); O0/O1/Os were
 * correct.  Fix: ir/opt_loop_const_sim.c — evaluate FP-flagged compares as real
 * float/double comparisons (lcs_evaluate_fp_compare).
 * Expected checksum is gcc -m32 -funsigned-char (ABI-independent here).
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  float f9 = -0x1.5a1a000000000p+11f;  /* negative: f9 <= (float)i is always 1 */
  for (unsigned g11 = 0u; g11 < 10u; g11++) {
    unsigned i10 = g11;
    cs = csmix(cs, i10);
    cs = csmix(cs, (((float)(f9)) <= ((float)((unsigned)(i10)))) ? 1u : 0u);
  }
  printf("checksum=%08x\n", cs);
  return 0;
}
