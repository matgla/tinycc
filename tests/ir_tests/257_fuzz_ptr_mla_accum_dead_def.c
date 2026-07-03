#include <stdio.h>

/*
 * Fuzz ptr seed 6869 reduction (O2): u7 = 846294235u | *p9 becomes a known
 * constant once store-load forwarding folds *p9, and every plain src1/src2
 * use of u7 is const-propagated away.  But `u7 + (u6 * u5)` was fused into
 * an MLA whose 4th (accumulator) operand at pool[operand_base+3] is
 * invisible to has_src1/has_src2 use-scans: const_var_prop/const_prop and
 * dse all treated u7's def as dead and NOP'd it, leaving the MLA reading an
 * undefined register.
 */
static unsigned csmix(unsigned h, unsigned v)
{
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(1686295559u) * (unsigned)(pa))) / ((unsigned)(3583280916u) | 1u))) * (unsigned)(((unsigned)(3186104708u) & (unsigned)(187076546u))))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(3603425707u) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s3 = (char)(266333914u & 0xff);
  long s4 = (long)(1456909217u & 0xffffffff);
  unsigned u5 = 3999990998u;
  unsigned u6 = 2178782463u;
  unsigned u7 = 4049835077u;
  unsigned arr8[8] = { 387709580u, 527784437u, 281719599u, 2956551060u, 570662476u, 3031281658u, 650564734u, 1680096368u };
  unsigned *p9 = &arr8[7u];
  unsigned *p10 = &arr8[7u];
  struct S st11 = { 3949136448u, 3856777198u, 2674263968u };
  struct S st12 = { 3771957391u, 1276557425u, 1864326676u };
  u7 = (unsigned)(((unsigned)(846294235u) | (unsigned)((*p9)))) & 0xffffffffu;
  cs = csmix(cs, *p9);
  cs = csmix(cs, *p10);
  cs = csmix(cs, (unsigned)((*p9)));
  cs = csmix(cs, *p10);
  cs = csmix(cs, *p9);
  u6 = (unsigned)(((unsigned)((-((unsigned)(2523312230u) | 0u))) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) << ((unsigned)((*p9)) & 31u))) + (unsigned)(((unsigned)(st12.f2) - (unsigned)(427989567u))))) == ((unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)(arr8[((unsigned)(u6) & 7u)]) | 1u))) ^ cs))) & 31u))) & 0xffffffffu;
  if ((unsigned)(((unsigned)(((unsigned)((-((unsigned)(((unsigned)(arr8[((unsigned)(2741328489u) & 7u)]) >> ((unsigned)(4249695087u) & 31u))) | 0u))) & (unsigned)(((unsigned)((-((unsigned)(4236805553u) | 0u))) << ((unsigned)(((unsigned)(u5) * (unsigned)((*p10)))) & 31u))))) >> ((unsigned)(((unsigned)(((unsigned)(helper2(u6, arr8[((unsigned)(3032644004u) & 7u)])) != ((unsigned)(((unsigned)(313877601u) >> ((unsigned)(arr8[((unsigned)(u7) & 7u)]) & 31u))) ^ cs))) + (unsigned)(((unsigned)(u7) + (unsigned)(((unsigned)(u6) * (unsigned)(u5))))))) & 31u))) & 1u) {
    if ((unsigned)((~((unsigned)(((unsigned)((((unsigned)(helper2((*p9), (unsigned)(s4))) & 1u) ? (unsigned)((unsigned)(s3)) : (unsigned)(helper2(u6, u7)))) - (unsigned)(((unsigned)((-((unsigned)((*p9)) | 0u))) & (unsigned)(((unsigned)(1983039808u) + (unsigned)(u5))))))) | 0u))) & 1u) {
      cs = csmix(cs, *p9);
      cs = csmix(cs, *p10);
      cs = csmix(cs, *p9);
      cs = csmix(cs, *p10);
      cs = csmix(cs, *p10);
      cs = csmix(cs, *p9);
    }
    cs = csmix(cs, (unsigned)(3702205406u));
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(u6) / ((unsigned)(((unsigned)(arr8[((unsigned)(734497548u) & 7u)]) >> ((unsigned)((~((unsigned)(1768358525u) | 0u))) & 31u))) | 1u))) >> ((unsigned)(((unsigned)(st12.f1) << ((unsigned)(u6) & 31u))) & 31u))));
    cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s4)) & (unsigned)(st12.f0))));
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) + (unsigned)(((unsigned)(u5) ^ cs)))) + (unsigned)(((unsigned)(st11.f2) - (unsigned)(u5))))) << ((unsigned)(u7) & 31u))) & (unsigned)(((unsigned)(((unsigned)(((unsigned)(949938456u) * (unsigned)(st11.f0))) + (unsigned)(((unsigned)((unsigned)(s3)) + (unsigned)(u5))))) / ((unsigned)(((unsigned)(((unsigned)(st11.f1) + (unsigned)((unsigned)(s3)))) & (unsigned)(((unsigned)(3883876594u) - (unsigned)(u6))))) | 1u))))));
    if ((unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) * (unsigned)(arr8[((unsigned)(678051977u) & 7u)]))) / ((unsigned)((~((unsigned)(st12.f1) | 0u))) | 1u))) ^ (unsigned)(st11.f0))) <= ((unsigned)(((unsigned)(((unsigned)(((unsigned)((*p9)) << ((unsigned)((unsigned)(s4)) & 31u))) >> ((unsigned)(((unsigned)(114170570u) ^ (unsigned)((*p9)))) & 31u))) * (unsigned)((~((unsigned)(u7) | 0u))))) ^ cs))) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) % ((unsigned)(u5) | 1u))) / ((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) * (unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)((*p10)) | 1u))))) * (unsigned)((-((unsigned)(((unsigned)((unsigned)(s3)) >= ((unsigned)(u6) ^ cs))) | 0u))))) | 1u))));
    }
    if ((unsigned)(463473461u) & 1u) {
      cs = csmix(cs, *p9);
      cs = csmix(cs, (unsigned)((((unsigned)(u6) & 1u) ? (unsigned)(((unsigned)(helper2(((unsigned)(u7) & (unsigned)(st12.f1)), ((unsigned)(1646108140u) << ((unsigned)(st11.f1) & 31u)))) | (unsigned)((*p9)))) : (unsigned)(705630251u))));
      cs = csmix(cs, *p10);
      cs = csmix(cs, (unsigned)((~((unsigned)(((unsigned)(((unsigned)(u6) ^ (unsigned)(((unsigned)(st11.f2) & (unsigned)(st11.f0))))) & (unsigned)(((unsigned)(helper1(u5, 78319647u)) * (unsigned)((unsigned)(s4)))))) | 0u))));
      u5 = (unsigned)((~((unsigned)(((unsigned)((~((unsigned)((~((unsigned)((unsigned)(s4)) | 0u))) | 0u))) - (unsigned)(u7))) | 0u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(helper2((~((unsigned)((*p9)) | 0u)), ((unsigned)(helper2((unsigned)(s3), (((unsigned)(st11.f0) & 1u) ? (unsigned)((unsigned)(s4)) : (unsigned)((*p10))))) >> ((unsigned)(((unsigned)(((unsigned)(u6) << ((unsigned)(st12.f0) & 31u))) << ((unsigned)((*p10)) & 31u))) & 31u)))));
    }
  }
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, st11.f0);
  cs = csmix(cs, st11.f1);
  cs = csmix(cs, st11.f2);
  cs = csmix(cs, st12.f0);
  cs = csmix(cs, st12.f1);
  cs = csmix(cs, st12.f2);
  cs = csmix(cs, *p9);
  cs = csmix(cs, *p10);
  printf("checksum=%08x\n", cs);
  return 0;
}
