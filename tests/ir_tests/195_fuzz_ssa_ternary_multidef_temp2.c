/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=118).
 * second repro of the multiply-defined ternary-result TEMP feeding an inlined parameter (see seed 100). Fix: ra_promote_multidef_temps_to_vars (ir/regalloc.c).
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
  lr = (unsigned)(((unsigned)(((unsigned)(((unsigned)(lr) + (unsigned)(((unsigned)(lr) ^ lr)))) % ((unsigned)(((unsigned)(3923455256u) | (unsigned)(3723764204u))) | 1u))) % ((unsigned)(3874325464u) | 1u)));
  if ((unsigned)((((unsigned)(3928592998u) & 1u) ? (unsigned)(pa) : (unsigned)(((unsigned)(2033844928u) / ((unsigned)(pa) | 1u))))) & 1u) lr += (unsigned)(((unsigned)(pb) << ((unsigned)(lr) & 31u)));
  lr = (unsigned)(((unsigned)(((unsigned)(((unsigned)(pa) << ((unsigned)(2474493012u) & 31u))) & (unsigned)(((unsigned)(lr) | (unsigned)(3664062795u))))) << ((unsigned)(700101179u) & 31u)));
  if ((unsigned)(1514496518u) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(993754430u) | (unsigned)(lr))) ^ (unsigned)(((unsigned)(pa) | (unsigned)(((unsigned)(pa) ^ lr))))));
  return (unsigned)(1863091875u) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  short s2 = (short)(1810557470u & 0xffff);
  char s3 = (char)(293113910u & 0xff);
  char s4 = (char)(2116948287u & 0xff);
  unsigned u5 = 2406199892u;
  unsigned u6 = 2781058702u;
  unsigned u7 = 2490833503u;
  unsigned u8 = 2609997502u;
  unsigned u9 = 1565476210u;
  struct S st10 = { 1690882216u, 88216819u, 2822966546u };

  if ((unsigned)((unsigned)(s2)) & 1u) {
    u7 = (unsigned)(u5) & 0xffffffffu;
  } else {
    st10.f2 = (unsigned)((unsigned)(s3));
  }
  { unsigned g12 = 0u;
    while (g12 < 9u) {
      unsigned i11 = g12;
      cs = csmix(cs, i11);
      st10.f1 = (unsigned)((((unsigned)(((unsigned)((unsigned)(s3)) % ((unsigned)((unsigned)(s4)) | 1u))) & 1u) ? (unsigned)(1617490812u) : (unsigned)(((unsigned)(((unsigned)(((unsigned)(44627584u) / ((unsigned)(st10.f1) | 1u))) ^ (unsigned)((unsigned)(s2)))) | (unsigned)((unsigned)(s4))))));
      u8 = (unsigned)(4148290143u) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(st10.f0));
      u7 = (unsigned)(((unsigned)((((unsigned)(((unsigned)(((unsigned)(i11) < ((unsigned)(467559988u) ^ cs))) >= ((unsigned)((-((unsigned)(1872960696u) | 0u))) ^ cs))) & 1u) ? (unsigned)(u6) : (unsigned)(3640208772u))) / ((unsigned)(804317891u) | 1u))) & 0xffffffffu;
      { unsigned g14 = 0u;
        while (g14 < 1u) {
          unsigned i13 = g14;
          cs = csmix(cs, i13);
          cs = csmix(cs, (unsigned)(2842670485u));
          cs = csmix(cs, (unsigned)((((unsigned)(u8) & 1u) ? (unsigned)(((unsigned)(1974078353u) % ((unsigned)(helper1((-((unsigned)(st10.f2) | 0u)), ((unsigned)(i13) & (unsigned)(((unsigned)(i13) ^ cs))))) | 1u))) : (unsigned)(((unsigned)((unsigned)(s3)) * (unsigned)(i11))))));
          cs = csmix(cs, (unsigned)((((unsigned)(u8) & 1u) ? (unsigned)((~((unsigned)(((unsigned)(2998086530u) + (unsigned)(((unsigned)(u5) ^ (unsigned)((unsigned)(s3)))))) | 0u))) : (unsigned)(((unsigned)(((unsigned)((~((unsigned)((unsigned)(s2)) | 0u))) >> ((unsigned)((-((unsigned)(st10.f2) | 0u))) & 31u))) & (unsigned)(st10.f2))))));
          u5 = (unsigned)((~((unsigned)(((unsigned)(894075551u) - (unsigned)((unsigned)(s2)))) | 0u))) & 0xffffffffu;
          g14++;
        }
      }
      { unsigned g16 = 0u;
        while (g16 < 9u) {
          unsigned i15 = g16;
          cs = csmix(cs, i15);
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(387090480u) << ((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)(1224592230u) | 1u))) & (unsigned)(((unsigned)((unsigned)(s3)) - (unsigned)(st10.f0))))) & 31u))) % ((unsigned)(((unsigned)((unsigned)(s3)) ^ (unsigned)((-((unsigned)(i15) | 0u))))) | 1u))));
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(u7) << ((unsigned)(((unsigned)(((unsigned)(3558523042u) ^ (unsigned)(st10.f0))) > ((unsigned)((-((unsigned)(i11) | 0u))) ^ cs))) & 31u))) ^ (unsigned)(381243630u))));
          i15 = (unsigned)(((unsigned)(((unsigned)((-((unsigned)(((unsigned)(u8) * (unsigned)(9273398u))) | 0u))) >> ((unsigned)(((unsigned)((unsigned)(s4)) % ((unsigned)(3862416649u) | 1u))) & 31u))) / ((unsigned)((((unsigned)(((unsigned)((unsigned)(s2)) ^ (unsigned)(((unsigned)(i11) % ((unsigned)(u8) | 1u))))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(u5) << ((unsigned)(u9) & 31u))) * (unsigned)(st10.f1))) : (unsigned)(((unsigned)((((unsigned)(u8) & 1u) ? (unsigned)(1681630285u) : (unsigned)(st10.f0))) >> ((unsigned)(((unsigned)(st10.f1) ^ (unsigned)(i15))) & 31u))))) | 1u))) & 0xffffffffu;
          g16++;
        }
      }
      g12++;
    }
  }
  st10.f1 = (unsigned)((unsigned)(s2));

  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
