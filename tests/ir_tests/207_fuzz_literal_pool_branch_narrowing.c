/* Regression for seed 809: a pending literal-pool flush could occur after
 * backward-branch narrowing chose a 16-bit conditional branch, moving the
 * source just out of T1 range and crashing backpatching. */
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
  short s1 = (short)(1312785637u & 0xffff);
  long s2 = (long)(1057476059u & 0xffffffff);
  unsigned u3 = 155963696u;
  unsigned u4 = 756353916u;
  unsigned u5 = 3395030125u;
  unsigned u6 = 819376322u;
  unsigned u7 = 3941239451u;
  struct S st8 = { 1474462013u, 3209438948u, 2571557252u };
  struct S st9 = { 1154885727u, 362159282u, 3967640674u };

  if ((unsigned)(((unsigned)(((unsigned)(3098026539u) >= ((unsigned)(((unsigned)(((unsigned)(u3) ^ (unsigned)(4285488969u))) >> ((unsigned)(((unsigned)(u7) * (unsigned)(st8.f2))) & 31u))) ^ cs))) << ((unsigned)(((unsigned)(2133601687u) < ((unsigned)(1512081449u) ^ cs))) & 31u))) & 1u) {
    u6 = (unsigned)(1323066731u) & 0xffffffffu;
    if ((unsigned)(2759121104u) & 1u) {
      u6 = (unsigned)(4048822399u) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s1)) ^ (unsigned)(((unsigned)(((unsigned)(1643990426u) ^ (unsigned)(2691856789u))) >> ((unsigned)(((unsigned)(((unsigned)(3025291642u) & (unsigned)((unsigned)(s1)))) & (unsigned)(((unsigned)(3962713117u) - (unsigned)(2189825254u))))) & 31u))))));
      u7 = (unsigned)(4120697097u) & 0xffffffffu;
      st8.f0 = (unsigned)(((unsigned)(((unsigned)(203483628u) / ((unsigned)(u6) | 1u))) & (unsigned)(1561056639u)));
      cs = csmix(cs, (unsigned)(((unsigned)(st8.f1) == ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) & (unsigned)(u4))) % ((unsigned)((unsigned)(s2)) | 1u))) % ((unsigned)(((unsigned)(4110422994u) % ((unsigned)(((unsigned)((unsigned)(s2)) / ((unsigned)(u3) | 1u))) | 1u))) | 1u))) ^ cs))));
      st9.f0 = (unsigned)(2160222603u);
    } else {
      cs = csmix(cs, (unsigned)(u6));
      cs = csmix(cs, (unsigned)(((unsigned)(u4) / ((unsigned)((((unsigned)(((unsigned)(((unsigned)(u4) * (unsigned)(u6))) / ((unsigned)(((unsigned)(u4) << ((unsigned)((unsigned)(s2)) & 31u))) | 1u))) & 1u) ? (unsigned)(((unsigned)(u3) > ((unsigned)(((unsigned)(234568644u) << ((unsigned)(u3) & 31u))) ^ cs))) : (unsigned)(4185889543u))) | 1u))));
      u7 = (unsigned)(((unsigned)(st8.f2) + (unsigned)(((unsigned)(564377100u) >> ((unsigned)(((unsigned)(((unsigned)(st8.f2) | (unsigned)(u6))) << ((unsigned)(st9.f2) & 31u))) & 31u))))) & 0xffffffffu;
      u6 = (unsigned)(u4) & 0xffffffffu;
    }
    u7 = (unsigned)((unsigned)(s1)) & 0xffffffffu;
  } else {
    if ((unsigned)(((unsigned)(((unsigned)(1095793161u) ^ (unsigned)(1187698588u))) % ((unsigned)(st8.f0) | 1u))) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) / ((unsigned)(u4) | 1u))) >> ((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) ^ (unsigned)(3008621946u))) + (unsigned)(st8.f0))) & 31u))) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) % ((unsigned)(st9.f2) | 1u))) | (unsigned)(((unsigned)(4231099461u) - (unsigned)(4006249249u))))) / ((unsigned)(((unsigned)(((unsigned)(st8.f1) << ((unsigned)(u7) & 31u))) | (unsigned)(u3))) | 1u))) & 31u))));
      st9.f2 = (unsigned)(((unsigned)(((unsigned)(3684840852u) << ((unsigned)(((unsigned)(st9.f0) | (unsigned)(((unsigned)(1211847898u) ^ (unsigned)(u3))))) & 31u))) | (unsigned)(324401378u)));
      u7 = (unsigned)(u4) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(st8.f0));
    } else {
      st9.f0 = (unsigned)((((unsigned)(((unsigned)(((unsigned)(2161667804u) * (unsigned)(u4))) >> ((unsigned)(1768730430u) & 31u))) & 1u) ? (unsigned)(1931756067u) : (unsigned)((~((unsigned)((unsigned)(s1)) | 0u)))));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(st9.f2) ^ (unsigned)(u4))) << ((unsigned)(2668459508u) & 31u))) & (unsigned)(((unsigned)(((unsigned)(u6) > ((unsigned)(u7) ^ cs))) << ((unsigned)(((unsigned)(u6) | (unsigned)((unsigned)(s1)))) & 31u))))) % ((unsigned)((~((unsigned)(((unsigned)((((unsigned)(u7) & 1u) ? (unsigned)(u5) : (unsigned)(3748049630u))) >> ((unsigned)(u7) & 31u))) | 0u))) | 1u))));
      st8.f0 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u6) >> ((unsigned)(4190292844u) & 31u))) / ((unsigned)((~((unsigned)(u3) | 0u))) | 1u))) & (unsigned)(st8.f1)));
      cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)((((unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(u6) & 31u))) & 1u) ? (unsigned)((~((unsigned)(u3) | 0u))) : (unsigned)(((unsigned)(4037592552u) ^ (unsigned)(3426488324u))))) | 0u))) + (unsigned)(((unsigned)(1072931480u) + (unsigned)(((unsigned)(((unsigned)(st8.f1) % ((unsigned)((unsigned)(s2)) | 1u))) >> ((unsigned)(((unsigned)(u4) << ((unsigned)(((unsigned)(u4) ^ cs)) & 31u))) & 31u))))))));
      cs = csmix(cs, (unsigned)((unsigned)(s1)));
    }
    u4 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(st9.f0) + (unsigned)(425479019u))) << ((unsigned)(((unsigned)((unsigned)(s1)) | (unsigned)((((unsigned)(u6) & 1u) ? (unsigned)((unsigned)(s1)) : (unsigned)(3790137879u))))) & 31u))) >> ((unsigned)(((unsigned)(((unsigned)(u3) / ((unsigned)(3779814139u) | 1u))) >> ((unsigned)(1460903456u) & 31u))) & 31u))) & 0xffffffffu;
    u6 = (unsigned)((-((unsigned)(u7) | 0u))) & 0xffffffffu;
    if ((unsigned)((-((unsigned)(1852483145u) | 0u))) & 1u) {
      u6 = (unsigned)((~((unsigned)(((unsigned)((~((unsigned)(1238522534u) | 0u))) ^ (unsigned)(((unsigned)(st8.f2) - (unsigned)(u3))))) | 0u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s2)) % ((unsigned)(((unsigned)(st9.f1) - (unsigned)(((unsigned)(((unsigned)(u7) >= ((unsigned)(3347254128u) ^ cs))) / ((unsigned)(u4) | 1u))))) | 1u))));
      u7 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(st9.f0) + (unsigned)(2273749039u))) & (unsigned)(((unsigned)(3474732049u) & (unsigned)(u5))))) | (unsigned)(((unsigned)(((unsigned)(u5) ^ (unsigned)(((unsigned)(u5) ^ cs)))) | (unsigned)(((unsigned)(u6) >> ((unsigned)(1825648263u) & 31u))))))) + (unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) / ((unsigned)(((unsigned)((unsigned)(s1)) / ((unsigned)(u4) | 1u))) | 1u))) - (unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) | (unsigned)(u3))) >> ((unsigned)(((unsigned)(1878555111u) ^ (unsigned)(st8.f1))) & 31u))))))) & 0xffffffffu;
    } else {
      u3 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(3395268792u) - (unsigned)(u3))) ^ (unsigned)((-((unsigned)(((unsigned)(u4) ^ (unsigned)(u7))) | 0u))))) / ((unsigned)(st8.f0) | 1u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)((-((unsigned)((unsigned)(s2)) | 0u))));
      cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(((unsigned)(((unsigned)(u3) << ((unsigned)(1763888950u) & 31u))) << ((unsigned)((-((unsigned)(st9.f0) | 0u))) & 31u))) | 0u))) * (unsigned)(3533261864u))));
      u6 = (unsigned)((((unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) + (unsigned)(3222144240u))) - (unsigned)(u5))) & 1u) ? (unsigned)((~((unsigned)(((unsigned)(u4) * (unsigned)(((unsigned)(u3) - (unsigned)(((unsigned)(u3) ^ cs)))))) | 0u))) : (unsigned)(u3))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(2107898357u));
    }
    for (unsigned g11 = 0u; g11 < 9u; g11++) {
      unsigned i10 = g11;
      cs = csmix(cs, i10);
      st8.f0 = (unsigned)(((unsigned)(2231625146u) << ((unsigned)(((unsigned)(((unsigned)(((unsigned)(st8.f0) & (unsigned)((unsigned)(s1)))) <= ((unsigned)((unsigned)(s2)) ^ cs))) - (unsigned)(((unsigned)(st8.f2) | (unsigned)(((unsigned)(st8.f2) ^ cs)))))) & 31u)));
      i10 = (unsigned)((((unsigned)(u5) & 1u) ? (unsigned)(((unsigned)(((unsigned)(1379859564u) + (unsigned)(((unsigned)(i10) - (unsigned)(2089837268u))))) % ((unsigned)((-((unsigned)(u3) | 0u))) | 1u))) : (unsigned)(3389075186u))) & 0xffffffffu;
    }
  }
  if ((unsigned)(((unsigned)(u4) * (unsigned)(((unsigned)(((unsigned)((~((unsigned)((unsigned)(s1)) | 0u))) * (unsigned)(((unsigned)((unsigned)(s1)) ^ (unsigned)((unsigned)(s2)))))) >> ((unsigned)((((unsigned)(((unsigned)(3258872106u) - (unsigned)(912125954u))) & 1u) ? (unsigned)(u5) : (unsigned)(4057490478u))) & 31u))))) & 1u) {
    u5 = (unsigned)(((unsigned)((((unsigned)(1671993034u) & 1u) ? (unsigned)((unsigned)(s1)) : (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(468606290u))) % ((unsigned)(3113433508u) | 1u))))) >> ((unsigned)((~((unsigned)(((unsigned)(1164121045u) >> ((unsigned)(((unsigned)(1146358495u) ^ (unsigned)(u7))) & 31u))) | 0u))) & 31u))) & 0xffffffffu;
    { unsigned g13 = 0u;
      while (g13 < 4u) {
        unsigned i12 = g13;
        cs = csmix(cs, i12);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) / ((unsigned)((~((unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(i12) & 31u))) | 0u))) | 1u))) ^ (unsigned)((unsigned)(s2)))));
        cs = csmix(cs, (unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(((unsigned)((((unsigned)(((unsigned)(st8.f0) * (unsigned)(u7))) & 1u) ? (unsigned)(((unsigned)(u6) + (unsigned)((unsigned)(s2)))) : (unsigned)(((unsigned)(i12) * (unsigned)(u3))))) | (unsigned)(((unsigned)((-((unsigned)(1187325619u) | 0u))) << ((unsigned)(((unsigned)(u4) >> ((unsigned)(3562560410u) & 31u))) & 31u))))) : (unsigned)(((unsigned)((-((unsigned)(((unsigned)(i12) % ((unsigned)(u7) | 1u))) | 0u))) % ((unsigned)((unsigned)(s1)) | 1u))))));
        u5 = (unsigned)(((unsigned)(((unsigned)(st8.f1) | (unsigned)((unsigned)(s2)))) + (unsigned)(i12))) & 0xffffffffu;
        g13++;
      }
    }
  }

  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, st8.f0);
  cs = csmix(cs, st8.f1);
  cs = csmix(cs, st8.f2);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
