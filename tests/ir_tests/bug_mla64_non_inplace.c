/* Regression: the mla-fusion pass (ir_gen_mla_fusion) rewrote a 64-bit
 * SMULL/UMULL feeding a 64-bit ADD into a 64-bit TCCIR_OP_MLA even when the
 * accumulate was NOT in place (the ADD destination is a fresh temp, not the
 * accumulator's own slot).  The only 64-bit MLA lowering is SMLAL/UMLAL, which
 * must accumulate into the destination register pair, so codegen aborted with
 * "compiler_error: unable to lower 64-bit MLA" (only at -O1+).
 *
 * `(long long)a + (long long)b * c` is exactly that non-in-place form.  Fixed
 * by only forming a 64-bit MLA when a store-back to the accumulator slot is
 * present (store_idx >= 0); otherwise it stays SMULL/UMULL + 64-bit ADD.
 */
#include <stdio.h>

static long long madd(int a, int b, int c) { return (long long)a + (long long)b * c; }

int main(void)
{
  long long t = 0;
  for (int i = 1; i <= 1000; i++)
    t += madd(i, i + 1, i - 1);
  printf("t=%lld\n", t);
  return (t == 334333000LL) ? 0 : 1;
}
