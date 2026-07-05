/* Regression for fuzz agg_deep seed 171000: MLA fusion must not expose stale
 * aggregate/pointer values to later copy and constant propagation. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
struct N { unsigned a; unsigned b; };
struct N2 { struct N n; unsigned t; };
int main(void)
{
  unsigned cs = 0x12345678u;
  long s1 = (long)(811394405u & 0xffffffff);
  unsigned u2 = 1196654296u;
  unsigned u3 = 3199673997u;
  unsigned u4 = 1633307917u;
  unsigned u5 = 1250486548u;
  unsigned arr6[8] = { 1812040890u, 2369452402u, 1045588298u, 1262419691u, 1438231944u, 3749378188u, 1588035191u, 981733996u };
  unsigned arr7[8] = { 1106264259u, 1818670878u, 4027290490u, 1580003773u, 4037405599u, 1841483186u, 1712451099u, 4169710570u };
  struct S st8 = { 1302540582u, 260812152u, 3874199084u };
  struct S st9 = { 3814930034u, 2641256411u, 2747381025u };
  struct N2 n210 = { { 1007146168u, 3294208790u }, 3699518404u };
  unsigned m211[4][4] = { { 4254631329u, 305145060u, 2769207606u, 3954157608u }, { 1443105416u, 3068071126u, 1516192549u, 275780136u }, { 2833307811u, 1061210489u, 758886027u, 1055451779u }, { 3416582305u, 1046568822u, 1923232448u, 2316206395u } };
  unsigned *pa212 = &u5;
  unsigned **ppa213 = &pa212;
  { unsigned g15 = 0u;
    while (g15 < 11u) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      if ((unsigned)((-((unsigned)((~((unsigned)(((unsigned)((-((unsigned)(n210.t) | 0u))) & (unsigned)(u5))) | 0u))) | 0u))) & 1u) {
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) % ((unsigned)(((unsigned)(((unsigned)(arr6[((unsigned)(u4) & 7u)]) | (unsigned)(m211[((unsigned)(1494978339u) & 3u)][((unsigned)(696714790u) & 3u)]))) * (unsigned)(m211[((unsigned)(u4) & 3u)][((unsigned)(499303973u) & 3u)]))) | 1u))) / ((unsigned)(n210.n.b) | 1u))));
        cs = csmix(cs, *(&m211[((unsigned)(1481976078u) & 3u)][0] + ((unsigned)(u2) & 3u)));
        n210.t = (unsigned)(((unsigned)(m211[((unsigned)(u2) & 3u)][((unsigned)(4133400003u) & 3u)]) % ((unsigned)(((unsigned)(i14) >> ((unsigned)((~((unsigned)(1140487998u) | 0u))) & 31u))) | 1u)));
        m211[((unsigned)(u2) & 3u)][((unsigned)(2552271511u) & 3u)] = (unsigned)(((unsigned)((~((unsigned)(n210.n.a) | 0u))) % ((unsigned)(((unsigned)(((unsigned)(((unsigned)(2252134723u) * (unsigned)(n210.n.b))) | (unsigned)(1821139729u))) >> ((unsigned)(arr7[((unsigned)(u2) & 7u)]) & 31u))) | 1u)));
        cs = csmix(cs, *(&m211[((unsigned)(u2) & 3u)][0] + ((unsigned)(2552271511u) & 3u)));
      }
      if ((unsigned)(((unsigned)(u2) << ((unsigned)(((unsigned)((**ppa213)) / ((unsigned)(((unsigned)(arr6[((unsigned)(977680821u) & 7u)]) % ((unsigned)(st8.f0) | 1u))) | 1u))) & 31u))) & 1u) {
        cs = csmix(cs, *(&m211[((unsigned)(i14) & 3u)][0] + ((unsigned)(3378651431u) & 3u)));
        cs = csmix(cs, (unsigned)(((unsigned)(arr6[((unsigned)(i14) & 7u)]) != ((unsigned)(((unsigned)(((unsigned)(((unsigned)(2130828990u) >> ((unsigned)(u3) & 31u))) / ((unsigned)(n210.n.b) | 1u))) | (unsigned)(((unsigned)(n210.n.a) / ((unsigned)(arr7[((unsigned)(i14) & 7u)]) | 1u))))) ^ cs))));
        cs = csmix(cs, **ppa213);
        cs = csmix(cs, *pa212);
        cs = csmix(cs, *(&m211[((unsigned)(1766972571u) & 3u)][0] + ((unsigned)(u2) & 3u)));
      }
      g15++;
    }
  }
  if ((unsigned)((~((unsigned)((**ppa213)) | 0u))) & 1u) {
    st9.f1 = (unsigned)(((unsigned)(((unsigned)((~((unsigned)((((unsigned)((**ppa213)) & 1u) ? (unsigned)(st9.f0) : (unsigned)((unsigned)(s1)))) | 0u))) << ((unsigned)(((unsigned)(((unsigned)(u4) << ((unsigned)(u5) & 31u))) % ((unsigned)(((unsigned)(st9.f2) < ((unsigned)(arr6[((unsigned)(u2) & 7u)]) ^ cs))) | 1u))) & 31u))) + (unsigned)(((unsigned)(((unsigned)(arr6[((unsigned)(u2) & 7u)]) << ((unsigned)((**ppa213)) & 31u))) * (unsigned)(((unsigned)(((unsigned)(m211[((unsigned)(3474702492u) & 3u)][((unsigned)(u4) & 3u)]) & (unsigned)(n210.t))) + (unsigned)(((unsigned)(arr6[((unsigned)(427570134u) & 7u)]) == ((unsigned)(st8.f0) ^ cs)))))))));
    for (unsigned g17 = 0u; g17 < 6u; g17++) {
      unsigned i16 = g17;
      cs = csmix(cs, i16);
    }
    for (unsigned g19 = 0u; g19 < 5u; g19++) {
      unsigned i18 = g19;
      cs = csmix(cs, i18);
      cs = csmix(cs, **ppa213);
      cs = csmix(cs, *pa212);
    }
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((**ppa213)) * (unsigned)(st9.f0))) < ((unsigned)(((unsigned)((**ppa213)) | (unsigned)(m211[((unsigned)(1932816024u) & 3u)][((unsigned)(2588746705u) & 3u)]))) ^ cs))) | (unsigned)(337481501u))) / ((unsigned)(u4) | 1u))));
    { unsigned g21 = 0u;
      while (g21 < 8u) {
        unsigned i20 = g21;
        cs = csmix(cs, i20);
        cs = csmix(cs, *(&m211[((unsigned)(2886855189u) & 3u)][0] + ((unsigned)(674196724u) & 3u)));
        cs = csmix(cs, *(&m211[((unsigned)(u2) & 3u)][0] + ((unsigned)(u4) & 3u)));
        cs = csmix(cs, *(&m211[((unsigned)(u3) & 3u)][0] + ((unsigned)(u4) & 3u)));
        g21++;
      }
    }
  }
  cs = csmix(cs, u2);
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, (unsigned)s1);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr6[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr7[k]);
  cs = csmix(cs, st8.f0);
  cs = csmix(cs, st8.f1);
  cs = csmix(cs, st8.f2);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  cs = csmix(cs, n210.n.a);
  cs = csmix(cs, n210.n.b);
  cs = csmix(cs, n210.t);
  for (unsigned ii = 0u; ii < 4u; ii++) for (unsigned jj = 0u; jj < 4u; jj++) cs = csmix(cs, m211[ii][jj]);
  cs = csmix(cs, **ppa213);
  cs = csmix(cs, *pa212);
  printf("checksum=%08x\n", cs);
  return 0;
}
