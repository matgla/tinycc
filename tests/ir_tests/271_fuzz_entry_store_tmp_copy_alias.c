/* Fuzz regression (agg_deep seed 12085; O1/O2 miscompile):
 * entry_store_prop's LEA map propagated stack addresses through
 * ASSIGN TEMP<-VAR, VAR<-TEMP and ADD, but not through a plain
 * ASSIGN TEMP<-TEMP pointer copy:
 *   T12 <-- Addr[StackLoc[-100]] ADD #48;  T15 <-- T12;  T15***DEREF*** <-- x
 * The store through T15 never invalidated the BLOCK_COPY initializer at that
 * offset, so a later read of arr[...] folded to the stale .rodata constant.
 * Fix: carry lea_map/rt_base through TEMP<-TEMP ASSIGN copies. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)((-((unsigned)(((unsigned)(((unsigned)(4188496303u) / ((unsigned)(pa) | 1u))) - (unsigned)(3577809403u))) | 0u))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)((((unsigned)(((unsigned)(408541632u) >> ((unsigned)(1939376377u) & 31u))) & 1u) ? (unsigned)(3145062055u) : (unsigned)(((unsigned)(2436327507u) * (unsigned)(lr))))) + (unsigned)(pb))) ^ lr;
}
static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)(2427870222u) ^ (unsigned)(((unsigned)(((unsigned)(1824963826u) >> ((unsigned)(lr) & 31u))) + (unsigned)(pb)))));
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(lr) + (unsigned)(pa))) | (unsigned)(lr))) / ((unsigned)(2030798384u) | 1u))) ^ lr;
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
  short s4 = (short)(437647918u & 0xffff);
  unsigned u5 = 3745632235u;
  unsigned u6 = 35056272u;
  unsigned u7 = 1202060202u;
  struct S st8 = { 2179848804u, 856962533u, 1470974143u };
  struct S st9 = { 3641190634u, 4027205172u, 3580121284u };
  struct N2 n210 = { { 6814230u, 1533286219u }, 1546252044u };
  unsigned m211[4][4] = { { 2976243698u, 1773916322u, 2464664441u, 96608698u }, { 99822359u, 33477985u, 3377360565u, 929013952u }, { 3913563947u, 3895297122u, 480193684u, 1395139810u }, { 2703344381u, 493502051u, 2279189805u, 2425612071u } };
  unsigned *pa212 = &u7;
  unsigned **ppa213 = &pa212;
  { unsigned g15 = 0u;
    while (g15 < 8u) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      cs = csmix(cs, *(&m211[((unsigned)(3518282432u) & 3u)][0] + ((unsigned)(u5) & 3u)));
      m211[((unsigned)(u5) & 3u)][((unsigned)(u6) & 3u)] = (unsigned)(((unsigned)(((unsigned)((((unsigned)(helper3(st9.f0, (unsigned)(s4))) & 1u) ? (unsigned)(((unsigned)((**ppa213)) * (unsigned)(2163707377u))) : (unsigned)(m211[((unsigned)(u7) & 3u)][((unsigned)(2250611289u) & 3u)]))) % ((unsigned)(st8.f2) | 1u))) | (unsigned)(668721444u)));
      cs = csmix(cs, *(&m211[((unsigned)(u5) & 3u)][0] + ((unsigned)(u6) & 3u)));
      if ((unsigned)(((unsigned)(((unsigned)(((unsigned)((**ppa213)) << ((unsigned)((~((unsigned)(m211[((unsigned)(1416316102u) & 3u)][((unsigned)(u5) & 3u)]) | 0u))) & 31u))) >> ((unsigned)(((unsigned)(((unsigned)(st9.f0) ^ (unsigned)(n210.n.a))) >> ((unsigned)(i14) & 31u))) & 31u))) % ((unsigned)(((unsigned)(((unsigned)(((unsigned)(i14) ^ (unsigned)(n210.n.b))) / ((unsigned)(n210.n.a) | 1u))) << ((unsigned)(((unsigned)((**ppa213)) / ((unsigned)(((unsigned)((**ppa213)) * (unsigned)(u6))) | 1u))) & 31u))) | 1u))) & 1u) {
        cs = csmix(cs, **ppa213);
        cs = csmix(cs, *pa212);
        cs = csmix(cs, **ppa213);
        cs = csmix(cs, *pa212);
      }
      g15++;
    }
  }
  if ((unsigned)((unsigned)(s4)) & 1u) {
    if ((unsigned)(((unsigned)(n210.n.b) % ((unsigned)(((unsigned)(n210.n.b) ^ cs)) | 1u))) & 1u) {
      cs = csmix(cs, *(&m211[((unsigned)(u6) & 3u)][0] + ((unsigned)(u5) & 3u)));
      u5 = (unsigned)(st9.f1) & 0xffffffffu;
      cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(u6) >= ((unsigned)(2602101849u) ^ cs))) & 1u) ? (unsigned)(((unsigned)(m211[((unsigned)(u5) & 3u)][((unsigned)(1458506980u) & 3u)]) % ((unsigned)(((unsigned)(1097392516u) - (unsigned)((unsigned)(s4)))) | 1u))) : (unsigned)(n210.n.b))));
    }
    cs = csmix(cs, **ppa213);
    cs = csmix(cs, *pa212);
    for (unsigned g17 = 0u; g17 < 8u; g17++) {
      unsigned i16 = g17;
      cs = csmix(cs, i16);
      cs = csmix(cs, *(&m211[((unsigned)(u5) & 3u)][0] + ((unsigned)(u6) & 3u)));
      cs = csmix(cs, (unsigned)((((unsigned)((**ppa213)) & 1u) ? (unsigned)(helper3(m211[((unsigned)(3898353942u) & 3u)][((unsigned)(i16) & 3u)], ((unsigned)(3851164576u) & (unsigned)((unsigned)(s4))))) : (unsigned)(((unsigned)(((unsigned)(m211[((unsigned)(2074672115u) & 3u)][((unsigned)(u7) & 3u)]) << ((unsigned)(n210.t) & 31u))) * (unsigned)(u7))))));
    }
    cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)((((unsigned)((**ppa213)) & 1u) ? (unsigned)(1032960570u) : (unsigned)(((unsigned)((**ppa213)) + (unsigned)(2574234313u))))) | 0u))) & (unsigned)(((unsigned)(((unsigned)((**ppa213)) > ((unsigned)(((unsigned)(n210.t) | (unsigned)(((unsigned)(n210.t) ^ cs)))) ^ cs))) <= ((unsigned)(((unsigned)(helper2((**ppa213), (**ppa213))) >= ((unsigned)(((unsigned)(570410556u) - (unsigned)(m211[((unsigned)(u7) & 3u)][((unsigned)(235773109u) & 3u)]))) ^ cs))) ^ cs))))));
  }
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
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
