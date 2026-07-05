/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=215).
 * SCCP constant-folded a barrel-shift-fused ALU op ignoring the hidden shift in ir->barrel_shifts[] (fix: ir/opt/ssa_opt_sccp.c).
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
  char s1 = (char)(427156591u & 0xff);
  char s2 = (char)(1020128146u & 0xff);
  int s3 = (int)(171261392u & 0xffffffff);
  unsigned u4 = 1614928261u;
  unsigned u5 = 2896190115u;
  unsigned u6 = 3482527559u;
  unsigned u7 = 3546624614u;
  unsigned u8 = 3412905721u;
  unsigned u9 = 1761875989u;
  unsigned arr10[8] = { 2457937844u, 3969618379u, 1066850956u, 929844199u, 942268185u, 221967792u, 3851245413u, 1067410679u };

  cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(((unsigned)(((unsigned)(526458343u) ^ (unsigned)(arr10[((unsigned)(u7) & 7u)]))) << ((unsigned)(((unsigned)(1882451124u) ^ (unsigned)((unsigned)(s1)))) & 31u))) <= ((unsigned)(286933763u) ^ cs))) & 1u) ? (unsigned)(arr10[((unsigned)(u8) & 7u)]) : (unsigned)((((unsigned)(((unsigned)(u8) * (unsigned)(((unsigned)(u5) - (unsigned)((unsigned)(s1)))))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(u7) >> ((unsigned)((unsigned)(s2)) & 31u))) + (unsigned)(((unsigned)(u5) ^ (unsigned)(u4))))) : (unsigned)(arr10[((unsigned)(1454116452u) & 7u)]))))));
  for (unsigned g12 = 0u; g12 < 3u; g12++) {
    unsigned i11 = g12;
    cs = csmix(cs, i11);
    u8 = (unsigned)(((unsigned)(((unsigned)((((unsigned)(((unsigned)(u8) ^ (unsigned)(1211865755u))) & 1u) ? (unsigned)(((unsigned)(2810816168u) / ((unsigned)((unsigned)(s3)) | 1u))) : (unsigned)(((unsigned)(u6) & (unsigned)(1704054934u))))) - (unsigned)(((unsigned)(402374130u) ^ (unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)((unsigned)(s3)) & 31u))))))) ^ (unsigned)((unsigned)(s1)))) & 0xffffffffu;
    u8 = (unsigned)(2733865422u) & 0xffffffffu;
    { unsigned g14 = 0u;
      while (g14 < 1u) {
        unsigned i13 = g14;
        cs = csmix(cs, i13);
        cs = csmix(cs, (unsigned)(3996467414u));
        arr10[((unsigned)(1228432826u) & 7u)] = (unsigned)(((unsigned)((((unsigned)(((unsigned)(((unsigned)(arr10[((unsigned)(3240595711u) & 7u)]) > ((unsigned)(u5) ^ cs))) - (unsigned)(((unsigned)(u6) - (unsigned)(i11))))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(3346976629u) | (unsigned)(u7))) & (unsigned)(3623486888u))) : (unsigned)(((unsigned)(3404307018u) + (unsigned)(u5))))) | (unsigned)(((unsigned)(arr10[((unsigned)(i13) & 7u)]) >> ((unsigned)(((unsigned)(u9) >> ((unsigned)((~((unsigned)(arr10[((unsigned)(i11) & 7u)]) | 0u))) & 31u))) & 31u)))));
        u8 = (unsigned)(u8) & 0xffffffffu;
        arr10[((unsigned)(936487338u) & 7u)] = (unsigned)(((unsigned)(arr10[((unsigned)(1076155346u) & 7u)]) | (unsigned)(((unsigned)(arr10[((unsigned)(1854764707u) & 7u)]) % ((unsigned)(arr10[((unsigned)(u8) & 7u)]) | 1u)))));
        cs = csmix(cs, (unsigned)(i11));
        i11 = (unsigned)(((unsigned)((-((unsigned)(u9) | 0u))) + (unsigned)(((unsigned)(329031271u) * (unsigned)((unsigned)(s3)))))) & 0xffffffffu;
        g14++;
      }
    }
  }
  arr10[((unsigned)(1481342091u) & 7u)] = (unsigned)(((unsigned)(((unsigned)(u4) ^ (unsigned)(((unsigned)(177185172u) == ((unsigned)(1539314884u) ^ cs))))) << ((unsigned)(arr10[((unsigned)(u7) & 7u)]) & 31u)));
  if ((unsigned)((unsigned)(s1)) & 1u) {
    u4 = (unsigned)(((unsigned)(u7) & (unsigned)(((unsigned)(((unsigned)(u9) - (unsigned)((~((unsigned)((unsigned)(s3)) | 0u))))) << ((unsigned)(676618279u) & 31u))))) & 0xffffffffu;
    arr10[((unsigned)(4111575762u) & 7u)] = (unsigned)((-((unsigned)(((unsigned)(((unsigned)((-((unsigned)(u5) | 0u))) % ((unsigned)(((unsigned)((unsigned)(s2)) + (unsigned)(4201977911u))) | 1u))) % ((unsigned)((unsigned)(s2)) | 1u))) | 0u)));
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) ^ (unsigned)(arr10[((unsigned)(445756443u) & 7u)]))) ^ (unsigned)(u8))) / ((unsigned)(3872512233u) | 1u))));
    u7 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(arr10[((unsigned)(2846972998u) & 7u)]) - (unsigned)(((unsigned)(1065517444u) ^ (unsigned)(u8))))) < ((unsigned)(u6) ^ cs))) - (unsigned)(((unsigned)(u7) | (unsigned)(((unsigned)(u7) >> ((unsigned)(((unsigned)(u7) ^ cs)) & 31u))))))) & 0xffffffffu;
    cs = csmix(cs, (unsigned)(2582394068u));
    u9 = (unsigned)(((unsigned)(((unsigned)(822103897u) & (unsigned)((unsigned)(s1)))) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(3729798219u) << ((unsigned)((unsigned)(s3)) & 31u))) >> ((unsigned)(((unsigned)(arr10[((unsigned)(u9) & 7u)]) / ((unsigned)(2272227488u) | 1u))) & 31u))) + (unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)(((unsigned)(3383843318u) >> ((unsigned)(u4) & 31u))) | 1u))))) & 31u))) & 0xffffffffu;
  }

  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr10[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
