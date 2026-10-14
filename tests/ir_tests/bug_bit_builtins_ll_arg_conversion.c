/* Guard for the __builtin_{clz,ctz,popcount,parity,ffs}{l,ll} argument
 * conversion.
 *
 * The ll forms call 64-bit helpers (__clzdi2, __ctzdi2, __popcountdi2,
 * __paritydi2, ffsll) that take unsigned long long in r0:r1.  Lowering used
 * an unprototyped call, so an int argument arrived in r0 with r1 left over
 * from earlier, and a long argument was not widened to 64 bits.  Cover: int
 * and long arguments to the ll helpers, the 32-bit forms, and ffs/ffsl/ffsll.
 *
 * The values are forced through volatile globals so the calls reach the
 * runtime helper path instead of folding at compile time.
 */
#include <stdio.h>

volatile int vi = -1, five = 5, one = 1;

int main(void)
{
  int i = vi, f5 = five, o = one;
  long li = vi;

  /* int argument to the 64-bit helpers: must be sign-extended to 64 bits. */
  if (__builtin_popcountll(i) != 64) return 1;
  if (__builtin_clzll(o) != 63) return 2;
  if (__builtin_parityll(i) != 0) return 3;
  if (__builtin_ctzll(i) != 0) return 4;
  if (__builtin_ffsll(i) != 1) return 5;
  if (__builtin_popcountll(f5) != 2) return 6;
  if (__builtin_clzll(f5) != 61) return 7;
  if (__builtin_parityll(f5) != 0) return 8;
  if (__builtin_ctzll(f5) != 0) return 9;
  if (__builtin_ffsll(f5) != 1) return 10;

  /* long argument to the 64-bit helpers: (long)-1 widens to all-ones. */
  if (__builtin_popcountll(li) != 64) return 11;
  if (__builtin_clzll(li) != 0) return 12;
  if (__builtin_ffsll(li) != 1) return 13;

  /* the 32-bit forms still work (int/long arguments). */
  if (__builtin_popcount(i) != 32) return 14;
  if (__builtin_clz(o) != 31) return 15;
  if (__builtin_parity(i) != 0) return 16;
  if (__builtin_ctz(i) != 0) return 17;
  if (__builtin_ffs(i) != 1) return 18;
  if (__builtin_popcountl(li) != 32) return 19;
  if (__builtin_clzl(o) != 31) return 20;
  if (__builtin_ffsl(li) != 1) return 21;

  printf("OK\n");
  return 0;
}
