// The value of an assignment to a volatile bitfield is the value stored --
// converted to the field, truncated and sign- or zero-extended -- and the
// field is not read again to produce it (C11 6.5.16p3; gcc does the same).
// tcc used to leave the field's lvalue as the result, so at -O0 even a plain
// `b->f2 = 3;` statement read the register a second time.
#include <stdio.h>

struct BF
{
  volatile unsigned u4 : 4;
  volatile int s4 : 4;
  volatile unsigned u24 : 24;
};
struct BB
{
  volatile _Bool b : 1;
  volatile unsigned long long q40 : 40;
  volatile long long s40 : 40;
};

struct BF bf;
struct BB bb;

__attribute__((noinline)) int pass(int x) { return x; }

int main(void)
{
  int a = (bf.u4 = 0x13);       /* truncates to 3 */
  int b = (bf.s4 = 15);         /* 0b1111 in a signed 4-bit field: -1 */
  int c = (bf.s4 = pass(7));    /* 7 fits */
  int d = (bf.s4 = pass(8));    /* wraps to -8 */
  unsigned e = (bf.u24 = 0x1234567u);
  int f = (bb.b = 2);           /* _Bool: 1 */
  unsigned long long g = (bb.q40 = 0x123456789abcull);
  long long h = (bb.s40 = 0xffffffffffll);
  int i, j;
  i = j = (bf.u4 = 0x1f);       /* chained: 15 into both */
  printf("a=%d b=%d c=%d d=%d e=%#x f=%d\n", a, b, c, d, e, f);
  printf("g=%#llx h=%lld i=%d j=%d\n", g, h, i, j);
  printf("u4=%u s4=%d u24=%#x b=%d q40=%#llx s40=%lld\n", bf.u4, bf.s4, bf.u24, bb.b,
         (unsigned long long)bb.q40, (long long)bb.s40);
  return 0;
}
