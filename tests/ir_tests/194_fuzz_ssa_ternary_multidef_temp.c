/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=100).
 * a side-effecting ternary (`cond ? helper() : x`) lowers to a TEMP written on both arms with no phi; SSA rename leaves it, so the merge use bound to one arm and an inlined-csmix use took it unconditionally. Fix: promote multiply-block-defined TEMPs to VARs before SSA construction (ir/regalloc.c).
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
  lr = (unsigned)(((unsigned)((((unsigned)(pb) & 1u) ? (unsigned)(((unsigned)(1958272647u) & (unsigned)(878258510u))) : (unsigned)(4126863879u))) ^ (unsigned)(((unsigned)(pb) << ((unsigned)(775874756u) & 31u)))));
  if ((unsigned)(((unsigned)(3875533785u) ^ (unsigned)(2616065593u))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(lr) >= ((unsigned)(pb) ^ lr))) % ((unsigned)(((unsigned)(lr) & (unsigned)(pb))) | 1u)));
  lr = (unsigned)((~((unsigned)(pa) | 0u)));
  lr = (unsigned)(((unsigned)(lr) >= ((unsigned)(((unsigned)(((unsigned)(lr) >> ((unsigned)(2440458082u) & 31u))) - (unsigned)(((unsigned)(2042330164u) + (unsigned)(898273845u))))) ^ lr)));
  if ((unsigned)((-((unsigned)(((unsigned)(lr) | (unsigned)(3010060546u))) | 0u))) & 1u) lr += (unsigned)(pa);
  return (unsigned)(((unsigned)(601206214u) >> ((unsigned)(((unsigned)(pa) & (unsigned)(pb))) & 31u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  long s2 = (long)(834093646u & 0xffffffff);
  char s3 = (char)(1794281797u & 0xff);
  short s4 = (short)(896277964u & 0xffff);
  unsigned u5 = 4248157111u;
  unsigned u6 = 3645445164u;
  unsigned u7 = 407768867u;
  unsigned u8 = 203365455u;
  unsigned u9 = 4099066840u;
  struct S st10 = { 3420726819u, 1264945112u, 1729389972u };

  u6 = (unsigned)(st10.f0) & 0xffffffffu;
  for (unsigned g12 = 0u; g12 < 6u; g12++) {
    unsigned i11 = g12;
    cs = csmix(cs, i11);
    st10.f2 = (unsigned)(((unsigned)((unsigned)(s3)) * (unsigned)(((unsigned)(st10.f2) | (unsigned)(((unsigned)(((unsigned)(u8) ^ (unsigned)(st10.f0))) < ((unsigned)(3969389376u) ^ cs)))))));
    st10.f1 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u8) >> ((unsigned)(((unsigned)(1896372016u) * (unsigned)(2430753581u))) & 31u))) | (unsigned)(((unsigned)(st10.f2) * (unsigned)(((unsigned)(630020755u) | (unsigned)(i11))))))) & (unsigned)(((unsigned)(u5) & (unsigned)(u6)))));
    for (unsigned g14 = 0u; g14 < 4u; g14++) {
      unsigned i13 = g14;
      cs = csmix(cs, i13);
      u6 = (unsigned)(((unsigned)(st10.f2) * (unsigned)(2114903742u))) & 0xffffffffu;
    }
    if ((unsigned)((unsigned)(s4)) & 1u) {
      u5 = (unsigned)(((unsigned)(((unsigned)(u7) + (unsigned)(u5))) >> ((unsigned)(helper1(((unsigned)(((unsigned)(u9) ^ (unsigned)(st10.f0))) << ((unsigned)(((unsigned)(874936471u) * (unsigned)(u6))) & 31u)), ((unsigned)((unsigned)(s2)) + (unsigned)(((unsigned)((unsigned)(s3)) - (unsigned)(u5)))))) & 31u))) & 0xffffffffu;
    } else {
      cs = csmix(cs, (unsigned)(((unsigned)(880388913u) | (unsigned)(u8))));
    }
    { unsigned g16 = 0u;
      while (g16 < 5u) {
        unsigned i15 = g16;
        cs = csmix(cs, i15);
        i11 = (unsigned)((unsigned)(s2)) & 0xffffffffu;
        i11 = (unsigned)((((unsigned)(((unsigned)(346808788u) << ((unsigned)(((unsigned)(((unsigned)(u8) >> ((unsigned)(st10.f2) & 31u))) / ((unsigned)(i11) | 1u))) & 31u))) & 1u) ? (unsigned)((unsigned)(s3)) : (unsigned)(((unsigned)(((unsigned)(((unsigned)(1137103299u) < ((unsigned)((unsigned)(s2)) ^ cs))) + (unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)((unsigned)(s4)) | 1u))))) >> ((unsigned)(((unsigned)((((unsigned)(u8) & 1u) ? (unsigned)(u6) : (unsigned)(3169409540u))) % ((unsigned)((~((unsigned)(3283581059u) | 0u))) | 1u))) & 31u))))) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(4138759898u) - (unsigned)(3417361263u))) >> ((unsigned)((unsigned)(s3)) & 31u))) ^ (unsigned)(((unsigned)(((unsigned)(370654763u) % ((unsigned)(u9) | 1u))) - (unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(u9) & 31u))))))) * (unsigned)((-((unsigned)((-((unsigned)((~((unsigned)(345242406u) | 0u))) | 0u))) | 0u))))));
        g16++;
      }
    }
    for (unsigned g18 = 0u; g18 < 1u; g18++) {
      unsigned i17 = g18;
      cs = csmix(cs, i17);
      cs = csmix(cs, (unsigned)(st10.f0));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) + (unsigned)(st10.f0))) % ((unsigned)(((unsigned)(i17) >> ((unsigned)(1760881623u) & 31u))) | 1u))) << ((unsigned)(u8) & 31u))) | (unsigned)(600183940u))));
      st10.f0 = (unsigned)((-((unsigned)(((unsigned)(2878020496u) + (unsigned)((unsigned)(s4)))) | 0u)));
    }
  }
  cs = csmix(cs, (unsigned)((unsigned)(s4)));
  for (unsigned g20 = 0u; g20 < 11u; g20++) {
    unsigned i19 = g20;
    cs = csmix(cs, i19);
    for (unsigned g22 = 0u; g22 < 7u; g22++) {
      unsigned i21 = g22;
      cs = csmix(cs, i21);
      u8 = (unsigned)(u5) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)(u7) - (unsigned)((unsigned)(s4)))));
      cs = csmix(cs, (unsigned)(1681369234u));
    }
    { unsigned g24 = 0u;
      while (g24 < 4u) {
        unsigned i23 = g24;
        cs = csmix(cs, i23);
        u7 = (unsigned)(st10.f2) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(((unsigned)((((unsigned)(((unsigned)(u6) >> ((unsigned)(helper1(1730728826u, st10.f2)) & 31u))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(st10.f2) == ((unsigned)(u9) ^ cs))) << ((unsigned)(((unsigned)(i19) % ((unsigned)(u7) | 1u))) & 31u))) : (unsigned)(2865608158u))) + (unsigned)(((unsigned)(u7) % ((unsigned)(((unsigned)(((unsigned)(3396685093u) == ((unsigned)((unsigned)(s3)) ^ cs))) / ((unsigned)(((unsigned)((unsigned)(s3)) % ((unsigned)(st10.f2) | 1u))) | 1u))) | 1u))))));
        st10.f2 = (unsigned)(u9);
        cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(((unsigned)(i23) & (unsigned)(((unsigned)(u5) & (unsigned)((unsigned)(s3)))))) & (unsigned)(3142299169u))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(((unsigned)(u6) * (unsigned)(736867629u))) | (unsigned)(i19))) ^ (unsigned)(((unsigned)(1438745272u) % ((unsigned)(helper1((unsigned)(s3), (unsigned)(s4))) | 1u))))) : (unsigned)(((unsigned)(u9) * (unsigned)(u5))))));
        cs = csmix(cs, (unsigned)(st10.f1));
        cs = csmix(cs, (unsigned)(st10.f1));
        g24++;
      }
    }
    if ((unsigned)((-((unsigned)(u9) | 0u))) & 1u) {
      u7 = (unsigned)(u5) & 0xffffffffu;
    } else {
      i19 = (unsigned)(((unsigned)(4285722225u) % ((unsigned)(((unsigned)(((unsigned)(st10.f2) << ((unsigned)(((unsigned)(u8) - (unsigned)(3011915941u))) & 31u))) % ((unsigned)(i19) | 1u))) | 1u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)(helper1((-((unsigned)(2357907037u) | 0u)), 72221122u)) * (unsigned)(((unsigned)(st10.f2) + (unsigned)((((unsigned)(3371920196u) & 1u) ? (unsigned)(st10.f1) : (unsigned)(i19))))))));
      cs = csmix(cs, (unsigned)((~((unsigned)(((unsigned)(((unsigned)(u6) * (unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(2235410371u))))) / ((unsigned)(((unsigned)(u8) << ((unsigned)(((unsigned)(u7) - (unsigned)((unsigned)(s3)))) & 31u))) | 1u))) | 0u))));
      u5 = (unsigned)(st10.f1) & 0xffffffffu;
    }
  }
  u9 = (unsigned)(((unsigned)(helper1(u6, ((unsigned)(((unsigned)((unsigned)(s2)) | (unsigned)(st10.f2))) % ((unsigned)(((unsigned)(3003544959u) >> ((unsigned)((unsigned)(s3)) & 31u))) | 1u)))) & (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(u8) & 31u))) % ((unsigned)(((unsigned)(1264080360u) + (unsigned)(u9))) | 1u))) + (unsigned)(u7))))) & 0xffffffffu;
  cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)(((unsigned)(st10.f2) & (unsigned)(165800416u))) | 0u))) >> ((unsigned)((unsigned)(s4)) & 31u))));

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
