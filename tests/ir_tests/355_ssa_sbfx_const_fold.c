/* ssa:fold  fold_sbfx — constant-fold a signed bitfield extract.
 *
 * When a narrowing signed cast `(int8_t)/(int16_t)x` has a constant source
 * (x becomes a compile-time constant only in SSA), it lowers to `#c SBFX #imm`.
 * fold_sbfx evaluates it with sign-extension, so the cast chain collapses to a
 * constant.  Without it, gcc-compile/20040304-2:foo stayed 119 instructions
 * (all ternary arms are casts of 0 = 0, so the whole body is dead) vs GCC's 1.
 *
 * Pins the sign-extension math across positive/negative byte and halfword.
 */
#include <stdio.h>

/* volatile sink defeats frontend literal folding, forcing the SBFX to appear
 * on an SSA-constant rather than a parse-time literal. */
volatile int seed = 0;

static int narrow8(int x)  { return (signed char)(x + seed); }
static int narrow16(int x) { return (short)(x + seed); }

int main(void)
{
  printf("%d %d %d %d %d %d\n",
         narrow8(200),    /* -56  */
         narrow8(5),      /*  5   */
         narrow8(-1),     /* -1   */
         narrow16(0x1234),/* 4660 */
         narrow16(0xFF80),/* -128 */
         narrow16(-1));   /* -1   */
  return 0;
}
