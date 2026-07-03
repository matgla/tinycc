/* Regression: loop-invariant const simulation dropped the sign of a narrow
 * (INT8/INT16) VAR when materializing its residual assignment.
 *
 * From struct_byval fuzz seed 4791 (reduced).  On this ARM target `char` is
 * unsigned, so `u = (unsigned)c;` for c == 254 must yield 254, not -2.
 *
 * The miscompile needed three passes to line up (loop-rotation + loop-unroll +
 * const-prop, each of which "fixed" it when disabled):
 *   1. loop_const_sim sinks the loop-invariant `u = c;` to `u <-- #254`, but
 *      LcsSlot tracked only the byte width (INT8), not is_unsigned, so the
 *      residual operand came out INT8 / signed.
 *   2. the post-unroll const-prop cleanup fits that constant to its operand via
 *      ir_opt_fit_const_to_operand, which sign-extends INT8 when is_unsigned=0,
 *      turning 254 (0xFE) into -2 (0xFFFFFFFE).
 *   3. that -2 then flows into the checksum, so tcc -O2 diverged from -O0/gcc.
 *
 * Fix: LcsSlot / LcsMemSlot carry is_unsigned, and the residual writeback
 * stamps it on the emitted operand so the deferred narrowing zero- vs
 * sign-extends correctly.  A signed-char arm is included to prove the fix
 * preserves BOTH signs, not just the unsigned one.
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
  unsigned char uc = (unsigned char)254;  /* 0xFE -> must stay 254 */
  signed char   sc = (signed char)200;    /* 0xC8 -> must stay -56  */
  unsigned uu = 1u;
  unsigned su = 1u;
  unsigned arr[8] = { 3221912432u, 3628209187u, 1643913241u, 1929035767u,
                      740816291u, 3769672840u, 1274608058u, 3443329166u };

  /* Loop-invariant stores of narrow-typed values: loop_const_sim sinks these
   * to residual constant assignments carrying the byte width. */
  for (unsigned g = 0u; g < 9u; g++) {
    uu = (unsigned)uc;
    su = (unsigned)sc;
  }
  cs = csmix(cs, uu);
  cs = csmix(cs, su);

  /* A second, fully-unrollable constant-trip loop so the post-unroll
   * const-prop cleanup runs (it is what folds the tainted residual). */
  for (unsigned k = 0u; k < 8u; k++)
    cs = csmix(cs, arr[k]);

  printf("checksum=%08x\n", cs);
  return 0;
}
