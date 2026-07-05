/* Regression for fuzz seed 155856: loop unrolling must not let later constant
 * propagation treat a stack-array element as its initializer after an indexed
 * store may have overwritten it. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s1 = (char)(562508744u & 0xff);
  short s2 = (short)(453713037u & 0xffff);
  int s3 = (int)(1321713993u & 0xffffffff);
  unsigned u4 = 1811895188u;
  unsigned u5 = 254895285u;
  unsigned arr6[8] = { 3741004670u, 253193817u, 1059062392u, 3631127781u, 1529555646u, 548198350u, 40919330u, 2473825440u };
  unsigned arr7[8] = { 2392944195u, 977439504u, 85879916u, 2661198209u, 1950718417u, 3448105614u, 81511772u, 512938734u };
  arr6[((unsigned)(u4) & 7u)] = (unsigned)(((unsigned)(((unsigned)((((unsigned)((((unsigned)((unsigned)(s1)) & 1u) ? (unsigned)(3719008108u) : (unsigned)((unsigned)(s3)))) & 1u) ? (unsigned)(((unsigned)(1447986788u) - (unsigned)(4032604743u))) : (unsigned)((~((unsigned)(u5) | 0u))))) >> ((unsigned)(2919083145u) & 31u))) > ((unsigned)(u4) ^ cs)));
  for (unsigned g9 = 0u; g9 < 10u; g9++) {
    unsigned i8 = g9;
    cs = csmix(cs, i8);
    cs = csmix(cs, (unsigned)(2471732250u));
    u4 = (unsigned)(arr6[((unsigned)(1116195372u) & 7u)]) & 0xffffffffu;
  }
  cs = csmix(cs, (unsigned)(arr6[((unsigned)(3353234457u) & 7u)]));
  cs = csmix(cs, (unsigned)(u4));
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr6[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr7[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
