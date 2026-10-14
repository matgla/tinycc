/* const_prop_tmp folded a shift whose constant count is outside [0, width)
 * with the host's C shift -- UB in the compiler (UBSan: shift exponent too
 * large / negative).  The program's shift is UB too, so only the in-range
 * cases are checked; this pins that the out-of-range ones still compile and
 * that in-range folds are unchanged at every level. */
#include <stdio.h>

__attribute__((noinline)) unsigned id(unsigned x) { return x; }

int main(void)
{
  volatile unsigned sink;
  unsigned a = 0x80000001u;
  int s = -8;
  printf("%08x %08x %08x %d\n", a << 4, a >> 4, (unsigned)((int)a >> 4), s >> 1);
  if (id(0)) /* never runs: shifts by 40 and -1 are UB, they only must compile */
  {
    sink = a << 40;
    sink = a >> -1;
  }
  printf("%u\n", id(3) << 31 >> 31);
  return 0;
}
