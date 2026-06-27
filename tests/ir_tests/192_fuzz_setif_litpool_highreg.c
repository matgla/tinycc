/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=89).
 * SETIF reserved only 6 literal-pool bytes; a high-register dest uses 4-byte mov.w so a pool flush split the ITE -> O1 HardFault (fix: arm-thumb-gen.c).
 * tcc -O0 was always correct; the bug appeared at -O1/-O2.  Expected checksum
 * is gcc -m32 -funsigned-char (ARM ABI: unsigned char, 32-bit long).
 */
#include <stdio.h>

/* Rolling checksum mix (all unsigned -> fully defined). */
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
  char s1 = (char)(603815478u & 0xff);
  char s2 = (char)(277796764u & 0xff);
  short s3 = (short)(1786798231u & 0xffff);
  unsigned u4 = 3847888817u;
  unsigned u5 = 2926314122u;
  unsigned u6 = 3695554968u;
  unsigned u7 = 15773094u;
  struct S st8 = { 3374269522u, 2398511342u, 390738240u };
  struct S st9 = { 1771282704u, 355166053u, 3595374514u };

  for (unsigned g11 = 0u; g11 < 3u; g11++) {
    unsigned i10 = g11;
    cs = csmix(cs, i10);
    u5 = (unsigned)(((unsigned)(2414466937u) * (unsigned)(((unsigned)(u7) % ((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) << ((unsigned)(i10) & 31u))) % ((unsigned)(((unsigned)(st9.f1) >> ((unsigned)(u5) & 31u))) | 1u))) | 1u))))) & 0xffffffffu;
  }
  if ((unsigned)((((unsigned)(((unsigned)((~((unsigned)(210798033u) | 0u))) + (unsigned)(((unsigned)((~((unsigned)(u7) | 0u))) / ((unsigned)(1337719038u) | 1u))))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(((unsigned)(2984267649u) % ((unsigned)(2798011457u) | 1u))) % ((unsigned)(((unsigned)(4287144937u) | (unsigned)(u4))) | 1u))) - (unsigned)(((unsigned)(((unsigned)(u6) / ((unsigned)(2667114879u) | 1u))) - (unsigned)(((unsigned)((unsigned)(s3)) & (unsigned)((unsigned)(s2)))))))) : (unsigned)(655126519u))) & 1u) {
    for (unsigned g13 = 0u; g13 < 9u; g13++) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      u7 = (unsigned)(((unsigned)(((unsigned)(((unsigned)((~((unsigned)(6391615u) | 0u))) / ((unsigned)(((unsigned)(st9.f0) % ((unsigned)(u7) | 1u))) | 1u))) ^ (unsigned)(1169949175u))) ^ (unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) != ((unsigned)(((unsigned)(u5) << ((unsigned)(465872607u) & 31u))) ^ cs))) - (unsigned)(((unsigned)(((unsigned)(st8.f0) >> ((unsigned)((unsigned)(s3)) & 31u))) << ((unsigned)(i12) & 31u))))))) & 0xffffffffu;
      i12 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(st9.f1) / ((unsigned)(3536294707u) | 1u))) / ((unsigned)(((unsigned)((~((unsigned)(2350230850u) | 0u))) != ((unsigned)((-((unsigned)(3795446749u) | 0u))) ^ cs))) | 1u))) ^ (unsigned)(st9.f0))) & 0xffffffffu;
      st9.f0 = (unsigned)((-((unsigned)(((unsigned)((~((unsigned)((unsigned)(s1)) | 0u))) / ((unsigned)((((unsigned)(st9.f1) & 1u) ? (unsigned)(((unsigned)(3269963256u) + (unsigned)(2432776134u))) : (unsigned)(((unsigned)((unsigned)(s3)) + (unsigned)(2447897140u))))) | 1u))) | 0u)));
      cs = csmix(cs, (unsigned)(((unsigned)(st8.f1) > ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) % ((unsigned)(1602902621u) | 1u))) + (unsigned)(u4))) != ((unsigned)(2847729693u) ^ cs))) ^ cs))));
      cs = csmix(cs, (unsigned)((-((unsigned)((~((unsigned)(61168703u) | 0u))) | 0u))));
      cs = csmix(cs, (unsigned)(3488086861u));
    }
  } else {
    for (unsigned g15 = 0u; g15 < 5u; g15++) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      cs = csmix(cs, (unsigned)(u6));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(2635082319u) | (unsigned)(((unsigned)(st8.f1) & (unsigned)(931406333u))))) & (unsigned)(((unsigned)(st8.f1) | (unsigned)(((unsigned)(st9.f1) >> ((unsigned)(((unsigned)(st9.f1) * (unsigned)(1654022632u))) & 31u))))))));
    }
    { unsigned g17 = 0u;
      while (g17 < 7u) {
        unsigned i16 = g17;
        cs = csmix(cs, i16);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) / ((unsigned)(((unsigned)(2146955459u) * (unsigned)(4147858903u))) | 1u))) - (unsigned)(((unsigned)(((unsigned)(i16) / ((unsigned)(st9.f2) | 1u))) - (unsigned)(u5))))) & (unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(u5) & 31u))))));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(st8.f2) - (unsigned)(u6))) % ((unsigned)(st9.f2) | 1u))) << ((unsigned)(st8.f0) & 31u))) | (unsigned)(((unsigned)(((unsigned)(3256380313u) | (unsigned)(u6))) ^ (unsigned)(st8.f0))))));
        u4 = (unsigned)((((unsigned)(((unsigned)(st9.f2) % ((unsigned)(4051051487u) | 1u))) & 1u) ? (unsigned)(((unsigned)(123629316u) - (unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(st8.f0) : (unsigned)((unsigned)(s2)))))) : (unsigned)(st8.f0))) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(u5));
        u4 = (unsigned)(u4) & 0xffffffffu;
        g17++;
      }
    }
    cs = csmix(cs, (unsigned)(2155550480u));
  }
  st8.f0 = (unsigned)(((unsigned)((-((unsigned)(((unsigned)(((unsigned)(1542664401u) * (unsigned)((unsigned)(s2)))) / ((unsigned)(((unsigned)(u5) - (unsigned)((unsigned)(s3)))) | 1u))) | 0u))) / ((unsigned)((unsigned)(s3)) | 1u)));
  { unsigned g19 = 0u;
    while (g19 < 11u) {
      unsigned i18 = g19;
      cs = csmix(cs, i18);
      if ((unsigned)(((unsigned)((~((unsigned)(((unsigned)((-((unsigned)((unsigned)(s1)) | 0u))) * (unsigned)(((unsigned)(u6) - (unsigned)((unsigned)(s1)))))) | 0u))) & (unsigned)(u6))) & 1u) {
        u4 = (unsigned)(((unsigned)((unsigned)(s3)) + (unsigned)(((unsigned)(((unsigned)(((unsigned)(st8.f0) == ((unsigned)((unsigned)(s1)) ^ cs))) / ((unsigned)(((unsigned)(i18) - (unsigned)(2771059005u))) | 1u))) & (unsigned)((unsigned)(s3)))))) & 0xffffffffu;
        u4 = (unsigned)(u5) & 0xffffffffu;
        u5 = (unsigned)(((unsigned)(((unsigned)(88742735u) % ((unsigned)((-((unsigned)(3478883072u) | 0u))) | 1u))) + (unsigned)(((unsigned)(((unsigned)(st8.f2) % ((unsigned)(((unsigned)(st9.f2) + (unsigned)(11523839u))) | 1u))) + (unsigned)(i18))))) & 0xffffffffu;
        u4 = (unsigned)(u7) & 0xffffffffu;
      } else {
        u7 = (unsigned)(524060708u) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(612462355u));
        u7 = (unsigned)(3581941356u) & 0xffffffffu;
      }
      cs = csmix(cs, (unsigned)(((unsigned)(591780480u) ^ (unsigned)(u6))));
      u5 = (unsigned)(((unsigned)((((unsigned)(((unsigned)((unsigned)(s3)) - (unsigned)(((unsigned)(i18) & (unsigned)(st8.f1))))) & 1u) ? (unsigned)(((unsigned)(u5) ^ (unsigned)(st8.f0))) : (unsigned)(((unsigned)(1440101678u) - (unsigned)(u6))))) + (unsigned)(((unsigned)(u4) ^ (unsigned)(st9.f1))))) & 0xffffffffu;
      { unsigned g21 = 0u;
        while (g21 < 11u) {
          unsigned i20 = g21;
          cs = csmix(cs, i20);
          st8.f2 = (unsigned)(u4);
          st8.f0 = (unsigned)(i18);
          i20 = (unsigned)(((unsigned)(2157257371u) - (unsigned)(1436741499u))) & 0xffffffffu;
          g21++;
        }
      }
      if ((unsigned)(((unsigned)(u5) - (unsigned)((-((unsigned)(st8.f0) | 0u))))) & 1u) {
        u7 = (unsigned)(((unsigned)(((unsigned)(u6) & (unsigned)(st9.f2))) / ((unsigned)(((unsigned)(((unsigned)(1846458793u) / ((unsigned)(((unsigned)((unsigned)(s2)) - (unsigned)(u7))) | 1u))) + (unsigned)(((unsigned)((~((unsigned)(st9.f1) | 0u))) * (unsigned)(1393090082u))))) | 1u))) & 0xffffffffu;
        cs = csmix(cs, (unsigned)((~((unsigned)(((unsigned)((unsigned)(s3)) * (unsigned)(1240996586u))) | 0u))));
        u7 = (unsigned)(st9.f2) & 0xffffffffu;
        u4 = (unsigned)(((unsigned)((((unsigned)(3765515127u) & 1u) ? (unsigned)(((unsigned)(2512906681u) < ((unsigned)(u7) ^ cs))) : (unsigned)(972926042u))) != ((unsigned)(((unsigned)((~((unsigned)(((unsigned)(3103907039u) + (unsigned)(1067447767u))) | 0u))) / ((unsigned)(2168176229u) | 1u))) ^ cs))) & 0xffffffffu;
        st8.f1 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(154862654u) ^ (unsigned)(st9.f2))) & (unsigned)(st9.f0))) | (unsigned)(st9.f1))) >> ((unsigned)(4244206878u) & 31u)));
      } else {
        u4 = (unsigned)(st9.f2) & 0xffffffffu;
      }
      g19++;
    }
  }
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) << ((unsigned)((-((unsigned)(u4) | 0u))) & 31u))) - (unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) | (unsigned)((unsigned)(s1)))) * (unsigned)((unsigned)(s3)))))) + (unsigned)(((unsigned)((unsigned)(s2)) / ((unsigned)(((unsigned)(((unsigned)(st8.f0) << ((unsigned)(u7) & 31u))) & (unsigned)(((unsigned)(u6) < ((unsigned)(639126933u) ^ cs))))) | 1u))))));
  cs = csmix(cs, (unsigned)(((unsigned)(u4) % ((unsigned)(((unsigned)(u4) ^ cs)) | 1u))));

  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, st8.f0);
  cs = csmix(cs, st8.f1);
  cs = csmix(cs, st8.f2);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
