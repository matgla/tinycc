/* ssa:bitop_const_fold — __builtin_clrsb/clrsbl/clrsbll (count leading redundant
 * sign bits) folded on a constant argument.
 *
 * The __builtin_clrsb* symbols already link against libtcc1 at runtime, so this
 * only closes the constant case (GCC folds it): __builtin_clrsb(CONST) was a
 * runtime helper call, now a compile-time constant.  -O2 folds, -O0 calls the
 * runtime helper; both must agree, so this pins the fold value AND the runtime
 * helper across the discriminating boundaries (INT_MIN, the l vs ll width, a
 * value whose set bits are all above bit 31, and clrsb of 0).
 */
#include <stdio.h>

volatile int vi;
volatile long long vll;

int main(void)
{
  /* constant-folded paths */
  printf("%d %d %d %d %d %d %d\n",
         __builtin_clrsb(0x00000f00),   /* 19 */
         __builtin_clrsb(-1),           /* 31 */
         __builtin_clrsb(0),            /* 31 */
         __builtin_clrsb(0x40000000),   /* 0  */
         __builtin_clrsb(0x55555555),   /* 0  */
         __builtin_clrsbl(0x80000000),  /* 0  (clrsbl is 32-bit: INT_MIN) */
         __builtin_clrsbl(0xf00));      /* 19 */
  printf("%d %d %d %d\n",
         (int)__builtin_clrsbll(0x100000000LL),        /* 30 (bits only above 31) */
         (int)__builtin_clrsbll(0),                    /* 63 */
         (int)__builtin_clrsbll(0x7FFFFFFFFFFFFFFFLL), /* 0  (INT64_MAX) */
         (int)__builtin_clrsbll(2LL));                 /* 61 */

  /* runtime paths (volatile defeats folding -> real libtcc1 helper) */
  vi = 0x00000f00;  int a = __builtin_clrsb(vi);       /* 19 */
  vi = 0x40000000;  int b = __builtin_clrsb(vi);       /* 0  */
  vll = 0x100000000LL; int c = __builtin_clrsbll(vll); /* 30 */
  vll = -1;         int d = __builtin_clrsbll(vll);    /* 63 */
  printf("%d %d %d %d\n", a, b, c, d);
  return 0;
}
