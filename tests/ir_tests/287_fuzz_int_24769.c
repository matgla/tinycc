/*
 * int fuzz seed 24769 reduction (O2 wrong-code):
 * loop rotation converted a top-tested counted loop whose body carried both
 * the checksum and another live VAR. Later O2 forwarding/threading over the
 * rotated shape propagated the wrong carried value. Rotation now rejects loop
 * bodies with multiple non-IV carried VARs; simple single-accumulator loops
 * can still rotate.
 * Ground truth (tcc -O0 == gcc -O2 on the original seed): checksum=09c2c297.
 * This reduced form's ground truth is checksum=2fe4d234.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)((-((unsigned)(((unsigned)(2013221088u) >= ((unsigned)(lr) ^ lr))) | 0u))) * (unsigned)(pa))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(1827955498u) ^ lr;
}
static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(pa) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(pa) % ((unsigned)(1575817169u) | 1u))) < ((unsigned)(pa) ^ lr)));
  return (unsigned)((-((unsigned)(((unsigned)(1665655750u) ^ (unsigned)(((unsigned)(pa) << ((unsigned)(3955765522u) & 31u))))) | 0u))) ^ lr;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s4 = (int)(1669722848u & 0xffffffff);
  short s5 = (short)(552126973u & 0xffff);
  char s6 = (char)(762396820u & 0xff);
  unsigned u7 = 206258941u;
  unsigned u8 = 631225112u;
  unsigned u9 = 832232781u;
  unsigned u10 = 483378658u;
  unsigned arr11[8] = { 450717457u, 2446967634u, 4000477749u, 2829127934u, 4022114436u, 4153694485u, 3058029744u, 576551044u };
  for (unsigned g13 = 0u; g13 < 11u; g13++) {
    unsigned i12 = g13;
    cs = csmix(cs, i12);
  }
  if ((unsigned)((~((unsigned)(((unsigned)(((unsigned)((~((unsigned)(arr11[((unsigned)(u7) & 7u)]) | 0u))) | (unsigned)(((unsigned)(510393009u) ^ (unsigned)(arr11[((unsigned)(1701737476u) & 7u)]))))) * (unsigned)(((unsigned)(((unsigned)(u8) >> ((unsigned)(3760877025u) & 31u))) ^ (unsigned)(u8))))) | 0u))) & 1u) {
    cs = csmix(cs, (unsigned)(((unsigned)(498827525u) == ((unsigned)(((unsigned)(((unsigned)((~((unsigned)(2523953997u) | 0u))) / ((unsigned)(((unsigned)(1802221784u) - (unsigned)(u8))) | 1u))) & (unsigned)(3275616894u))) ^ cs))));
    for (unsigned g15 = 0u; g15 < 8u; g15++) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      cs = csmix(cs, (unsigned)(((unsigned)(u8) % ((unsigned)(((unsigned)(773657430u) % ((unsigned)((-((unsigned)(u7) | 0u))) | 1u))) | 1u))));
      arr11[((unsigned)(u8) & 7u)] = (unsigned)(((unsigned)(1473648239u) & (unsigned)(((unsigned)(i14) << ((unsigned)(arr11[((unsigned)(u7) & 7u)]) & 31u)))));
    }
    for (unsigned g17 = 0u; g17 < 5u; g17++) {
      unsigned i16 = g17;
      cs = csmix(cs, i16);
      cs = csmix(cs, (unsigned)((unsigned)(s4)));
      u9 = (unsigned)(((unsigned)((-((unsigned)(775230455u) | 0u))) | (unsigned)(u9))) & 0xffffffffu;
    }
    if ((unsigned)(arr11[((unsigned)(3297877615u) & 7u)]) & 1u) {
      u9 = (unsigned)(914835850u) & 0xffffffffu;
    }
    u7 = (unsigned)(288360608u) & 0xffffffffu;
  }
  if ((unsigned)(((unsigned)((((unsigned)((~((unsigned)(u7) | 0u))) & 1u) ? (unsigned)(((unsigned)((unsigned)(s5)) & (unsigned)(arr11[((unsigned)(71397334u) & 7u)]))) : (unsigned)(196312127u))) >= ((unsigned)(((unsigned)((((unsigned)(((unsigned)((unsigned)(s6)) % ((unsigned)(3170721986u) | 1u))) & 1u) ? (unsigned)(1078930223u) : (unsigned)(u10))) >> ((unsigned)(u8) & 31u))) ^ cs))) & 1u) {
    u8 = (unsigned)((((unsigned)(((unsigned)(((unsigned)((~((unsigned)(u8) | 0u))) + (unsigned)(((unsigned)((unsigned)(s6)) ^ (unsigned)(3525240574u))))) + (unsigned)((((unsigned)(u10) & 1u) ? (unsigned)((((unsigned)(arr11[((unsigned)(397706671u) & 7u)]) & 1u) ? (unsigned)(arr11[((unsigned)(1058573174u) & 7u)]) : (unsigned)(u10))) : (unsigned)(u8))))) & 1u) ? (unsigned)(u9) : (unsigned)((-((unsigned)(1554389536u) | 0u))))) & 0xffffffffu;
    for (unsigned g19 = 0u; g19 < 3u; g19++) {
      unsigned i18 = g19;
      cs = csmix(cs, i18);
      u10 = (unsigned)(helper3((((unsigned)(((unsigned)(u10) & (unsigned)(((unsigned)(u8) + (unsigned)((unsigned)(s4)))))) & 1u) ? (unsigned)((((unsigned)((-((unsigned)((unsigned)(s5)) | 0u))) & 1u) ? (unsigned)((unsigned)(s4)) : (unsigned)((((unsigned)(u8) & 1u) ? (unsigned)((unsigned)(s5)) : (unsigned)(arr11[((unsigned)(u8) & 7u)]))))) : (unsigned)(u9)), ((unsigned)((-((unsigned)(3143898688u) | 0u))) % ((unsigned)(((unsigned)(((unsigned)(u9) & (unsigned)(1930429413u))) % ((unsigned)(((unsigned)(i18) / ((unsigned)(3438936076u) | 1u))) | 1u))) | 1u)))) & 0xffffffffu;
      u9 = (unsigned)(helper2(u7, ((unsigned)(((unsigned)(i18) > ((unsigned)(arr11[((unsigned)(u8) & 7u)]) ^ cs))) + (unsigned)((unsigned)(s6))))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)(u7) + (unsigned)(((unsigned)(u7) ^ cs)))));
    }
    cs = csmix(cs, (unsigned)((unsigned)(s6)));
    if ((unsigned)(((unsigned)(((unsigned)(u7) + (unsigned)(arr11[((unsigned)(u9) & 7u)]))) | (unsigned)((unsigned)(s5)))) & 1u) {
      cs = csmix(cs, (unsigned)((unsigned)(s5)));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(699526540u) <= ((unsigned)((unsigned)(s5)) ^ cs))) >> ((unsigned)(((unsigned)(u8) << ((unsigned)(u9) & 31u))) & 31u))) >> ((unsigned)(((unsigned)(((unsigned)((unsigned)(s6)) * (unsigned)(arr11[((unsigned)(u10) & 7u)]))) | (unsigned)((~((unsigned)(arr11[((unsigned)(u8) & 7u)]) | 0u))))) & 31u))) ^ (unsigned)(((unsigned)(((unsigned)(((unsigned)(arr11[((unsigned)(u8) & 7u)]) & (unsigned)(391080879u))) << ((unsigned)((unsigned)(s6)) & 31u))) % ((unsigned)(((unsigned)(((unsigned)(3182269420u) >> ((unsigned)(u10) & 31u))) < ((unsigned)(helper3(arr11[((unsigned)(u10) & 7u)], 2310328037u)) ^ cs))) | 1u))))));
    }
    if ((unsigned)(arr11[((unsigned)(u7) & 7u)]) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s4)) * (unsigned)((unsigned)(s6)))));
    }
  }
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(1458627470u) & (unsigned)((unsigned)(s5)))) < ((unsigned)(((unsigned)(3731073929u) <= ((unsigned)(u8) ^ cs))) ^ cs))) < ((unsigned)((-((unsigned)((-((unsigned)(arr11[((unsigned)(680855830u) & 7u)]) | 0u))) | 0u))) ^ cs))) + (unsigned)(1635541675u))));
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, u10);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  cs = csmix(cs, (unsigned)s6);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr11[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
