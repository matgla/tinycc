/* Regression for seed 814: var_tmp_fwd must not extend a TEMP across an
 * intervening VAR store in the store-heavy csmix shape.  The over-forwarded
 * form combined with redundant_assign/late cleanup miscompiled at -O2. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(pb) + (unsigned)(lr))) + (unsigned)(pb))) + (unsigned)(((unsigned)(3460450790u) - (unsigned)(((unsigned)(1283187553u) - (unsigned)(pb))))))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s2 = (char)(1246023176u & 0xff);
  unsigned u3 = 2848001808u;
  unsigned u4 = 3105512058u;
  unsigned u5 = 3051641225u;
  struct S st6 = { 1995161129u, 102738848u, 547486504u };
  struct S st7 = { 2010102770u, 388860190u, 1434759544u };
  u4 = (unsigned)((((unsigned)(((unsigned)(1227157542u) >> ((unsigned)(217987903u) & 31u))) & 1u) ? (unsigned)(u3) : (unsigned)(3257212801u))) & 0xffffffffu;
  if ((unsigned)(((unsigned)(246017389u) >> ((unsigned)((-((unsigned)((-((unsigned)(((unsigned)(u4) - (unsigned)(((unsigned)(u4) ^ cs)))) | 0u))) | 0u))) & 31u))) & 1u) {
    { unsigned g9 = 0u;
      while (g9 < 1u) {
        unsigned i8 = g9;
        cs = csmix(cs, i8);
        u3 = (unsigned)(((unsigned)(((unsigned)(st7.f1) * (unsigned)(((unsigned)(i8) < ((unsigned)(3624286130u) ^ cs))))) * (unsigned)((-((unsigned)(2694452480u) | 0u))))) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(((unsigned)(2646141539u) & (unsigned)(((unsigned)(i8) << ((unsigned)(((unsigned)(u3) - (unsigned)(759629791u))) & 31u))))));
        u4 = (unsigned)(88938692u) & 0xffffffffu;
        g9++;
      }
    }
  } else {
    { unsigned g11 = 0u;
      while (g11 < 1u) {
        unsigned i10 = g11;
        cs = csmix(cs, i10);
        cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(((unsigned)(((unsigned)(3790685568u) & (unsigned)(u4))) | (unsigned)(((unsigned)(1666983493u) - (unsigned)(4252156823u))))) | 0u))) << ((unsigned)(((unsigned)(((unsigned)(((unsigned)(1661748267u) <= ((unsigned)(i10) ^ cs))) - (unsigned)(((unsigned)(i10) & (unsigned)(st7.f1))))) + (unsigned)(helper1(((unsigned)(u3) ^ (unsigned)(i10)), ((unsigned)((unsigned)(s2)) | (unsigned)(((unsigned)((unsigned)(s2)) ^ cs))))))) & 31u))));
        cs = csmix(cs, (unsigned)(u5));
        g11++;
      }
    }
    for (unsigned g13 = 0u; g13 < 10u; g13++) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      cs = csmix(cs, (unsigned)(3158793947u));
    }
    { unsigned g15 = 0u;
      while (g15 < 9u) {
        unsigned i14 = g15;
        cs = csmix(cs, i14);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(st7.f0) << ((unsigned)(st7.f1) & 31u))) >> ((unsigned)(1323476042u) & 31u))));
        g15++;
      }
    }
    { unsigned g17 = 0u;
      while (g17 < 6u) {
        unsigned i16 = g17;
        cs = csmix(cs, i16);
        g17++;
      }
    }
  }
  { unsigned g19 = 0u;
    while (g19 < 6u) {
      unsigned i18 = g19;
      cs = csmix(cs, i18);
      if ((unsigned)(helper1(helper1(((unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(u4) & 31u))) << ((unsigned)(((unsigned)(u3) >> ((unsigned)(2845152014u) & 31u))) & 31u)), ((unsigned)(u4) - (unsigned)((~((unsigned)(1735905975u) | 0u))))), ((unsigned)(u5) & (unsigned)((((unsigned)((unsigned)(s2)) & 1u) ? (unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(((unsigned)((unsigned)(s2)) ^ cs)) & 31u))) : (unsigned)(((unsigned)(i18) % ((unsigned)(u3) | 1u)))))))) & 1u) {
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(182248616u) / ((unsigned)(st7.f0) | 1u))) ^ (unsigned)(st6.f0))));
        cs = csmix(cs, (unsigned)((unsigned)(s2)));
      }
      cs = csmix(cs, (unsigned)(helper1(st6.f1, (unsigned)(s2))));
      cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)(helper1(helper1((unsigned)(s2), 1674363878u), 4221965481u)) | 0u))) >> ((unsigned)(3124637258u) & 31u))));
      g19++;
    }
  }
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((~((unsigned)(u5) | 0u))) - (unsigned)(((unsigned)(3281513315u) | (unsigned)((unsigned)(s2)))))) | (unsigned)(2807144531u))) ^ (unsigned)(helper1(((unsigned)((~((unsigned)((unsigned)(s2)) | 0u))) - (unsigned)(((unsigned)(593997020u) < ((unsigned)(u4) ^ cs)))), ((unsigned)(helper1(u5, 827471841u)) & (unsigned)(((unsigned)(3538027201u) - (unsigned)(u4)))))))));
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, st6.f0);
  cs = csmix(cs, st6.f1);
  cs = csmix(cs, st6.f2);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
