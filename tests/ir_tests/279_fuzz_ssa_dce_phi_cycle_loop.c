/*
 * fp_round fuzz seed 18960 reduction (O1): ssa:dce:phi_cycles removed phis
 * inside a loop body whose values still had to be carried by out-of-SSA phi
 * resolution, corrupting the loop-carried checksum state.
 * Ground truth (tcc -O0 == tcc -O2): checksum=ee90ea2b.
 */
#include <stdio.h>
#include <string.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}
static unsigned fbits_d(double d){ unsigned u[2]; memcpy(u, &d, sizeof u); return csmix(u[0], u[1]); }
static unsigned fbits_f(float f){ unsigned u; memcpy(&u, &f, sizeof u); return u; }
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s1 = (int)(1165141899u & 0xffffffff);
  long s2 = (long)(1361187397u & 0xffffffff);
  int s3 = (int)(1595303655u & 0xffffffff);
  unsigned u4 = 4131004568u;
  unsigned u5 = 149638564u;
  unsigned u6 = 49267363u;
  unsigned arr7[8] = { 2253771132u, 552310346u, 3132868911u, 3177618005u, 3487900738u, 3892926072u, 2646356661u, 2329586594u };
  unsigned arr8[8] = { 1638168037u, 3138974493u, 308296051u, 4081971127u, 1509640278u, 2739694052u, 807190446u, 289230212u };
  struct S st9 = { 3430946765u, 2012343687u, 2831087663u };
  double f10 = 0x1.f68a400000000p+14;
  float f11 = -0x1.7249160000000p+12f;
  float f12 = 0x1.f1de3e0000000p+17f;
  double f13 = 0x1.b3e8ee0000000p+43;
  { unsigned g15 = 0u;
    while (g15 < 12u) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(2032864482u) * (unsigned)(u4))) | (unsigned)(((unsigned)(arr7[((unsigned)(1773899832u) & 7u)]) % ((unsigned)(arr8[((unsigned)(u5) & 7u)]) | 1u))))) << ((unsigned)(((unsigned)(((unsigned)(arr7[((unsigned)(3454036299u) & 7u)]) & (unsigned)(2938017620u))) / ((unsigned)(u6) | 1u))) & 31u))) | (unsigned)(((unsigned)(((unsigned)((-((unsigned)(3849533954u) | 0u))) ^ (unsigned)(((unsigned)(u4) + (unsigned)(3191659886u))))) ^ (unsigned)((unsigned)(s2)))))));
      f11 = (f11 < -0x1p40f || f11 > 0x1p40f) ? (float)1 : f11;
      { unsigned g17 = 0u;
        while (g17 < 6u) {
          unsigned i16 = g17;
          cs = csmix(cs, i16);
          u4 = (unsigned)(((unsigned)(u4) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(1896228734u) % ((unsigned)(arr7[((unsigned)(u5) & 7u)]) | 1u))) - (unsigned)(((unsigned)(u4) * (unsigned)(arr8[((unsigned)(u5) & 7u)]))))) << ((unsigned)(((unsigned)((((unsigned)(i16) & 1u) ? (unsigned)(u6) : (unsigned)(((unsigned)(u6) ^ cs)))) & (unsigned)(((unsigned)(3423377966u) % ((unsigned)(i16) | 1u))))) & 31u))) & 31u))) & 0xffffffffu;
          cs = csmix(cs, (unsigned)(u5));
          g17++;
        }
      }
      cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(st9.f1) * (unsigned)((((unsigned)(((unsigned)(u5) % ((unsigned)((unsigned)(s3)) | 1u))) & 1u) ? (unsigned)(st9.f2) : (unsigned)(((unsigned)(u5) % ((unsigned)(u4) | 1u))))))) & 1u) ? (unsigned)(((unsigned)(st9.f1) | (unsigned)((-((unsigned)(((unsigned)(arr7[((unsigned)(u6) & 7u)]) | (unsigned)(i14))) | 0u))))) : (unsigned)(st9.f0))));
      g15++;
    }
  }
  f12 = (f12 < -0x1p40f || f12 > 0x1p40f) ? (float)1 : f12;
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr7[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  cs = csmix(cs, fbits_d(f10));
  cs = csmix(cs, fbits_f(f11));
  cs = csmix(cs, fbits_f(f12));
  cs = csmix(cs, fbits_d(f13));
  printf("checksum=%08x\n", cs);
  return 0;
}
