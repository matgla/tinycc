/* A register alive_share lent out is not free just because its OWNER spilled.
 *
 * ra:alive_share puts a second live holder on a register the allocator still
 * shows as taken by the first -- that is the whole point of it.  Eviction then
 * picked the owner as its spill victim and handed the register to a new
 * interval, but spilling the owner does NOT free the register: the borrower is
 * still living there, and its value is destroyed between its definition and
 * its use.  The expire path states the invariant ("only free a register when
 * no surviving interval still holds it"); eviction had no equivalent.
 *
 * Measured on this program at -O2: alive_share lends R12 to a temp over
 * [105,122] while the merge temp owns it across [102,126]; eviction spills the
 * owner and gives R12 away over [115,119].
 *
 * It takes real register pressure and an if/else merge whose arms both define
 * the same value, which is why this is a fuzz reduction (longlong seed 197)
 * rather than something written by hand.  Same bug: longlong 1449 and
 * combo_num 1103. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  short s1 = (short)(90829728u & 0xffff);
  unsigned u2 = 2504597986u;
  unsigned u3 = 2081379782u;
  unsigned u4 = 2681507826u;
  unsigned long long q5 = (((unsigned long long)(u4)) << 32) | (unsigned long long)(u3);
  unsigned long long q6 = (((unsigned long long)(u3)) << 32) | (unsigned long long)(u4);
  unsigned long long q7 = (((unsigned long long)(u4)) << 32) | (unsigned long long)(u3);
  unsigned long long q8 = (((unsigned long long)(u4)) << 32) | (unsigned long long)(u3);
  struct S st9 = { 3078485621u, 1041743909u, 1029069764u };
  cs = csmix(cs, (unsigned)(q7) ^ (unsigned)(q7 >> 32));
  cs = csmix(cs, (unsigned)((unsigned)(s1)));
  if ((unsigned)(st9.f1) & 1u) {
    if ((unsigned)((((unsigned)(u2) & 1u) ? (unsigned)(u3) : (unsigned)(((unsigned)(((unsigned)((((unsigned)(3746058814u) & 1u) ? (unsigned)(3458201546u) : (unsigned)(u3))) ^ (unsigned)(1343907409u))) / ((unsigned)(st9.f1) | 1u))))) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s1)) << ((unsigned)(((unsigned)((unsigned)(s1)) < ((unsigned)(u4) ^ cs))) & 31u))));
    } else {
      u2 = (unsigned)(((unsigned)(u2) + (unsigned)(u3))) & 0xffffffffu;
      q7 = (((unsigned long long)(unsigned)(((unsigned)((~((unsigned)(u4) | 0u))) | (unsigned)(((unsigned)(u2) % ((unsigned)(u4) | 1u))))))) >> ((unsigned)(((unsigned)((unsigned)(s1)) & (unsigned)(2490539759u))) & 63u);
    }
    u4 = (unsigned)(((unsigned)(st9.f0) << ((unsigned)(u2) & 31u))) & 0xffffffffu;
    if ((unsigned)(((unsigned)(((unsigned)(u2) - (unsigned)(((unsigned)(((unsigned)(1770957371u) & (unsigned)(u2))) - (unsigned)(st9.f1))))) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(423991143u) / ((unsigned)(3712597894u) | 1u))) - (unsigned)(((unsigned)(2595007699u) >> ((unsigned)((unsigned)(s1)) & 31u))))) >> ((unsigned)(((unsigned)(((unsigned)(3369889190u) | (unsigned)(u3))) % ((unsigned)(((unsigned)(u4) | (unsigned)(((unsigned)(u4) ^ cs)))) | 1u))) & 31u))) & 31u))) & 1u) {
      u2 = (unsigned)((-((unsigned)(((unsigned)(u4) ^ (unsigned)(((unsigned)((-((unsigned)(st9.f1) | 0u))) & (unsigned)(((unsigned)(st9.f2) - (unsigned)(3397749729u))))))) | 0u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((((unsigned)(((unsigned)(st9.f1) | (unsigned)(st9.f0))) & 1u) ? (unsigned)((unsigned)(s1)) : (unsigned)(((unsigned)((unsigned)(s1)) + (unsigned)(((unsigned)((unsigned)(s1)) ^ cs)))))) >> ((unsigned)(((unsigned)(((unsigned)(2073984964u) - (unsigned)(2749828640u))) ^ (unsigned)((-((unsigned)(u3) | 0u))))) & 31u))) == ((unsigned)(u4) ^ cs))));
    }
  } else {
    cs = csmix(cs, (unsigned)(((unsigned)(u3) % ((unsigned)(((unsigned)(((unsigned)(((unsigned)(2079564487u) + (unsigned)(u2))) % ((unsigned)((((unsigned)(st9.f0) & 1u) ? (unsigned)(u3) : (unsigned)(u4))) | 1u))) >> ((unsigned)((~((unsigned)(u3) | 0u))) & 31u))) | 1u))));
    for (unsigned g11 = 0u; g11 < 5u; g11++) {
      unsigned i10 = g11;
    }
    for (unsigned g13 = 0u; g13 < 5u; g13++) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      cs = csmix(cs, (unsigned)(q8) ^ (unsigned)(q8 >> 32));
      cs = csmix(cs, (unsigned)((((unsigned)(3709886646u) & 1u) ? (unsigned)(((unsigned)(((unsigned)(i12) ^ (unsigned)(((unsigned)(i12) ^ cs)))) * (unsigned)((-((unsigned)(((unsigned)((unsigned)(s1)) - (unsigned)(1283018761u))) | 0u))))) : (unsigned)(((unsigned)(u2) < ((unsigned)(((unsigned)(st9.f0) | (unsigned)(((unsigned)(265134004u) & (unsigned)(2867800018u))))) ^ cs))))));
      cs = csmix(cs, ((q8) <= (((q8) ^ (unsigned long long)(cs)))) ? 1u : 0u);
      cs = csmix(cs, (unsigned)(u2));
    }
  }
  cs = csmix(cs, ((q7) == (q5)) ? 1u : 0u);
  cs = csmix(cs, u2);
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, (unsigned)(q5) ^ (unsigned)(q5 >> 32));
  cs = csmix(cs, (unsigned)(q6) ^ (unsigned)(q6 >> 32));
  cs = csmix(cs, (unsigned)(q7) ^ (unsigned)(q7 >> 32));
  cs = csmix(cs, (unsigned)(q8) ^ (unsigned)(q8 >> 32));
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  printf("checksum=%08x\n", cs);
}
