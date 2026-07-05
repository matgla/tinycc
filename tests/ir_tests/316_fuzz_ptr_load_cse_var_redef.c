/* fuzz longlong seed 188167 / struct_byval seed 182993 (O1/O2 divergence).
 * Pass: ptr_load_cse (ir/opt_memory.c).
 * Root cause: the copy-CSE cache keyed `T <- V` lval-VAR reads by the source
 * VAR and forwarded a later read of the same VAR to the earlier temp — but it
 * only flushed that cache on a STORE opcode or a register-form (is_lval=0)
 * write.  A local VAR reassigned via the *memory-form* ASSIGN `V0 <- #const`
 * (is_lval=1) slipped past both, so the post-reassignment read of V0 was
 * re-CSE'd to the pre-reassignment value.  Here the dead loop-carried multiply
 * `q7 = 7*q7` reads q7 at the loop top, then `q7 = <const>` reassigns it; the
 * `q7 * 3` that follows re-read q7 and was wrongly folded back to the loop-top
 * value, turning the loop-invariant `q7 = const*3` into `q7 = 3^6`.
 * Fix: also invalidate cache entries keyed on a VAR when that VAR is written
 * by an is_lval ASSIGN (memory-form slot store).
 */
#include <stdio.h>
int main(void)
{
  unsigned long long q7 = 1ull;
  for (unsigned g = 0u; g < 6u; g++) {
    q7 = (unsigned long long)7u * q7;   /* dead: overwritten below */
    q7 = 0x1111111122222222ull;
    q7 = q7 * 0x3ull;
  }
  unsigned cs = (unsigned)q7 ^ (unsigned)(q7 >> 32);
  printf("checksum=%08x\n", cs);
  return 0;
}
