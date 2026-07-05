/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=151).
 * mach_mod_mop materialized an immediate dividend into the divisor's register: src2 was not pre-excluded before src1 (fix: arm-thumb-gen.c).
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


static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)((((unsigned)(((unsigned)(((unsigned)(660994688u) & (unsigned)(753280586u))) ^ (unsigned)(((unsigned)(lr) - (unsigned)(pa))))) & 1u) ? (unsigned)(lr) : (unsigned)(3464618271u)));
  lr = (unsigned)(((unsigned)(pa) % ((unsigned)(lr) | 1u)));
  if ((unsigned)((-((unsigned)(((unsigned)(pb) & (unsigned)(3082336196u))) | 0u))) & 1u) lr += (unsigned)(2025068101u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(pa) - (unsigned)(lr))) + (unsigned)(((unsigned)(2349648253u) % ((unsigned)(lr) | 1u))))) << ((unsigned)(((unsigned)(lr) & (unsigned)(((unsigned)(pb) / ((unsigned)(4110652638u) | 1u))))) & 31u))) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(((unsigned)(lr) + (unsigned)(((unsigned)(lr) % ((unsigned)(1088500497u) | 1u))))) & 1u) lr += (unsigned)((-((unsigned)(2789607883u) | 0u)));
  if ((unsigned)(3348745851u) & 1u) lr += (unsigned)(3949886429u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(3501076978u) >> ((unsigned)(2728237937u) & 31u))) ^ (unsigned)(pa))) / ((unsigned)(((unsigned)(((unsigned)(1072640217u) << ((unsigned)(4216318152u) & 31u))) | (unsigned)(((unsigned)(2400147273u) >> ((unsigned)(1375463094u) & 31u))))) | 1u))) ^ lr;
}

static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(1917523498u);
  lr = (unsigned)(((unsigned)(pb) < ((unsigned)(((unsigned)(pa) * (unsigned)(((unsigned)(pb) | (unsigned)(pa))))) ^ lr)));
  if ((unsigned)(((unsigned)(((unsigned)(3002176328u) + (unsigned)(pb))) / ((unsigned)(((unsigned)(pb) / ((unsigned)(214665388u) | 1u))) | 1u))) & 1u) lr += (unsigned)(1958170367u);
  lr = (unsigned)(3040564988u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(983521429u) & (unsigned)(pb))) / ((unsigned)(((unsigned)(3385556405u) << ((unsigned)(pb) & 31u))) | 1u))) >> ((unsigned)(pb) & 31u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  int s4 = (int)(2062270065u & 0xffffffff);
  unsigned u5 = 1312100418u;
  unsigned u6 = 11876832u;
  unsigned arr7[8] = { 242568587u, 614401957u, 3823656897u, 2894886288u, 974531402u, 112182435u, 1819216209u, 2029481942u };
  unsigned arr8[8] = { 1937396006u, 1818353465u, 3492670893u, 132493509u, 2267404571u, 730438130u, 924013506u, 1359943497u };
  struct S st9 = { 1622244425u, 1848486413u, 1114277475u };
  struct S st10 = { 2025074052u, 3465422376u, 1327513312u };

  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(arr7[((unsigned)(3053965582u) & 7u)]) - (unsigned)(arr7[((unsigned)(3660756146u) & 7u)]))) & (unsigned)(u6))));
  { unsigned g12 = 0u;
    while (g12 < 10u) {
      unsigned i11 = g12;
      cs = csmix(cs, i11);
      u5 = (unsigned)(((unsigned)((unsigned)(s4)) * (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) ^ (unsigned)(u5))) << ((unsigned)(((unsigned)(((unsigned)(arr7[((unsigned)(u6) & 7u)]) << ((unsigned)(u6) & 31u))) + (unsigned)(((unsigned)(arr7[((unsigned)(i11) & 7u)]) + (unsigned)(3588470833u))))) & 31u))))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)(arr7[((unsigned)(u6) & 7u)]) >= ((unsigned)(arr7[((unsigned)(i11) & 7u)]) ^ cs))));
      { unsigned g14 = 0u;
        while (g14 < 5u) {
          unsigned i13 = g14;
          cs = csmix(cs, i13);
          i13 = (unsigned)((((unsigned)(((unsigned)(helper3(arr8[((unsigned)(i13) & 7u)], i11)) <= ((unsigned)(((unsigned)(1683205476u) >> ((unsigned)(((unsigned)(i11) - (unsigned)(420581287u))) & 31u))) ^ cs))) & 1u) ? (unsigned)(((unsigned)((-((unsigned)(((unsigned)((unsigned)(s4)) % ((unsigned)(u5) | 1u))) | 0u))) + (unsigned)(st10.f0))) : (unsigned)(((unsigned)((unsigned)(s4)) - (unsigned)(((unsigned)((((unsigned)(4157646948u) & 1u) ? (unsigned)(st10.f2) : (unsigned)(i11))) >= ((unsigned)((-((unsigned)(993910095u) | 0u))) ^ cs))))))) & 0xffffffffu;
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(st9.f1) / ((unsigned)(i11) | 1u))) << ((unsigned)((((unsigned)(u5) & 1u) ? (unsigned)(arr7[((unsigned)(i13) & 7u)]) : (unsigned)(i13))) & 31u))) >> ((unsigned)(((unsigned)((unsigned)(s4)) >> ((unsigned)(st10.f0) & 31u))) & 31u))) + (unsigned)((~((unsigned)(((unsigned)((~((unsigned)(u6) | 0u))) | (unsigned)(((unsigned)(139351843u) - (unsigned)(st9.f0))))) | 0u))))));
          g14++;
        }
      }
      if ((unsigned)(((unsigned)((-((unsigned)(((unsigned)(st10.f2) << ((unsigned)(((unsigned)(arr8[((unsigned)(u5) & 7u)]) | (unsigned)(st9.f1))) & 31u))) | 0u))) + (unsigned)((unsigned)(s4)))) & 1u) {
        st10.f1 = (unsigned)(((unsigned)(helper1(((unsigned)(((unsigned)(u5) & (unsigned)(i11))) + (unsigned)(((unsigned)(i11) & (unsigned)(u6)))), ((unsigned)(i11) ^ (unsigned)(((unsigned)(818829736u) != ((unsigned)(st9.f2) ^ cs)))))) - (unsigned)(1966769778u)));
        cs = csmix(cs, (unsigned)(((unsigned)((((unsigned)(st9.f0) & 1u) ? (unsigned)(((unsigned)(((unsigned)(st10.f2) & (unsigned)((unsigned)(s4)))) != ((unsigned)((~((unsigned)(st10.f1) | 0u))) ^ cs))) : (unsigned)(arr8[((unsigned)(2083610041u) & 7u)]))) + (unsigned)(((unsigned)(helper3((-((unsigned)(u6) | 0u)), ((unsigned)(3331755628u) | (unsigned)(st9.f1)))) % ((unsigned)((~((unsigned)(((unsigned)(arr8[((unsigned)(i11) & 7u)]) & (unsigned)((unsigned)(s4)))) | 0u))) | 1u))))));
        cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(((unsigned)(i11) & (unsigned)(((unsigned)(3386519204u) | (unsigned)(arr8[((unsigned)(u6) & 7u)]))))) | 0u))) << ((unsigned)((((unsigned)(((unsigned)(((unsigned)(arr7[((unsigned)(u6) & 7u)]) - (unsigned)(i11))) ^ (unsigned)(((unsigned)(u5) | (unsigned)(((unsigned)(u5) ^ cs)))))) & 1u) ? (unsigned)(1394021262u) : (unsigned)(((unsigned)(((unsigned)(u6) / ((unsigned)(3402410314u) | 1u))) << ((unsigned)(((unsigned)(i11) != ((unsigned)(((unsigned)(i11) ^ cs)) ^ cs))) & 31u))))) & 31u))));
        arr8[((unsigned)(u5) & 7u)] = (unsigned)(((unsigned)(3450507160u) & (unsigned)(((unsigned)(helper3(((unsigned)(4135429323u) % ((unsigned)(st9.f0) | 1u)), arr7[((unsigned)(u6) & 7u)])) >> ((unsigned)(((unsigned)(((unsigned)(u6) & (unsigned)(arr8[((unsigned)(u5) & 7u)]))) & (unsigned)(i11))) & 31u)))));
        i11 = (unsigned)(helper2((unsigned)(s4), ((unsigned)(((unsigned)(u5) | (unsigned)(((unsigned)(3191053396u) & (unsigned)(739726335u))))) >> ((unsigned)(u6) & 31u)))) & 0xffffffffu;
        arr7[((unsigned)(i11) & 7u)] = (unsigned)(((unsigned)((~((unsigned)(((unsigned)((~((unsigned)(st10.f0) | 0u))) & (unsigned)((unsigned)(s4)))) | 0u))) - (unsigned)((-((unsigned)(arr8[((unsigned)(2777875191u) & 7u)]) | 0u)))));
      }
      g12++;
    }
  }
  st10.f2 = (unsigned)(((unsigned)(((unsigned)((-((unsigned)((unsigned)(s4)) | 0u))) ^ (unsigned)(helper3(1412595050u, u5)))) & (unsigned)(((unsigned)((unsigned)(s4)) * (unsigned)(466064709u)))));
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(arr7[((unsigned)(u6) & 7u)]) - (unsigned)(((unsigned)(2626081658u) >> ((unsigned)(st10.f2) & 31u))))) % ((unsigned)((unsigned)(s4)) | 1u))) % ((unsigned)(((unsigned)(u6) ^ (unsigned)(((unsigned)(((unsigned)(arr8[((unsigned)(u5) & 7u)]) <= ((unsigned)(u6) ^ cs))) / ((unsigned)(arr8[((unsigned)(1689581271u) & 7u)]) | 1u))))) | 1u))));
  cs = csmix(cs, (unsigned)(u6));
  cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(2641899765u) & (unsigned)(st9.f2))) & 1u) ? (unsigned)((((unsigned)((((unsigned)(3429254102u) & 1u) ? (unsigned)(((unsigned)(u6) - (unsigned)(arr7[((unsigned)(u5) & 7u)]))) : (unsigned)((~((unsigned)(1388483779u) | 0u))))) & 1u) ? (unsigned)(((unsigned)(u6) * (unsigned)(helper3(1757658664u, st9.f0)))) : (unsigned)(helper1((((unsigned)(1326533726u) & 1u) ? (unsigned)(1296459285u) : (unsigned)(st9.f0)), 1174821237u)))) : (unsigned)((((unsigned)(((unsigned)(u6) << ((unsigned)(((unsigned)(st10.f1) & (unsigned)(u5))) & 31u))) & 1u) ? (unsigned)(helper2(arr7[((unsigned)(u5) & 7u)], arr8[((unsigned)(3782668456u) & 7u)])) : (unsigned)(4218139496u))))));

  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr7[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
