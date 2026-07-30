/* ssa:bitop_const_fold — fold a __builtin bit-op with a constant argument.
 *
 * tcc lowers __builtin_popcount/clz/ctz/ffs/parity(const) to a runtime helper
 * call (__popcountsi2 etc.); this pass evaluates it at compile time, as GCC
 * does (gcc-execute/builtin-bitops-1's unrolled TEST block of constant checks).
 *
 * Pins correctness across ops and 32/64-bit widths.  The printed values are
 * the folded constants; a wrong fold changes the output.
 */
#include <stdio.h>

int main(void)
{
  printf("%d %d %d %d %d %d %d %d %d\n",
         __builtin_popcount(0xa5a5a5a5u),          /* 16 */
         __builtin_clz(0x00010000u),               /* 15 */
         __builtin_ctz(0x00010000u),               /* 16 */
         __builtin_ffs(0x00000050),                /*  5 */
         __builtin_ffs(0),                          /*  0 */
         __builtin_parity(0xa5a5a5a5u),            /*  0 */
         (int)__builtin_popcountll(0xa5a5a5a5a5a5a5a5ull), /* 32 */
         (int)__builtin_ctzll(0x0000000100000000ull),      /* 32 */
         (int)__builtin_clzll(0x0000000000000001ull));     /* 63 */
  return 0;
}
