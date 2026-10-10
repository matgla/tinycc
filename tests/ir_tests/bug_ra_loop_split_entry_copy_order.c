/* Regression test (differential-fuzz repro, gen_c.py seed=172, reduced with
 * scripts/reduce_divergence.py). ra:loop_split inserted a loop entry copy
 * `T <- V` BEFORE the def group's loop-forced write-back `V <- T2` when both
 * share the insertion point right after the def (the entry edge falls through
 * from the def block), so T read V with no reaching def.  Fix: ra_split_copy_cmp
 * sorts entry copies after write-backs at a shared point.  The bug appeared at
 * -O2 with the pass on (now default-on); expected checksum is gcc -m32 -funsigned-char.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(pa) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(2068595881u) ^ (unsigned)(((unsigned)(2632887526u) | (unsigned)(lr))))) & (unsigned)(((unsigned)((-((unsigned)(2006434513u) | 0u))) / ((unsigned)(961604557u) | 1u))))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s3 = (char)(1798434177u & 0xff);
  unsigned u4 = 2740668307u;
  unsigned u5 = 2640349046u;
  unsigned arr6[8] = { 1435362365u, 4256371693u, 1258254303u, 3035698507u, 3646110234u, 1683257748u, 2923263323u, 584325404u };
  struct S st7 = { 4209762761u, 1071270617u, 14844717u };
  struct S st8 = { 4027070499u, 267737776u, 747981435u };
  cs = csmix(cs, (unsigned)(((unsigned)(st8.f0) | (unsigned)(((unsigned)(((unsigned)((~((unsigned)(u5) | 0u))) >> ((unsigned)(((unsigned)((unsigned)(s3)) * (unsigned)(st8.f1))) & 31u))) - (unsigned)(st8.f0))))));
  cs = csmix(cs, (unsigned)(((unsigned)(u4) ^ (unsigned)(((unsigned)(((unsigned)(st8.f1) >> ((unsigned)((~((unsigned)((unsigned)(s3)) | 0u))) & 31u))) + (unsigned)((~((unsigned)((((unsigned)(arr6[((unsigned)(3886695079u) & 7u)]) & 1u) ? (unsigned)(1802676641u) : (unsigned)(arr6[((unsigned)(u4) & 7u)]))) | 0u))))))));
  if ((unsigned)(((unsigned)((-((unsigned)(((unsigned)(((unsigned)(u4) ^ (unsigned)(410460036u))) << ((unsigned)(u5) & 31u))) | 0u))) * (unsigned)(((unsigned)(1227079886u) % ((unsigned)(((unsigned)((-((unsigned)(arr6[((unsigned)(u4) & 7u)]) | 0u))) >> ((unsigned)((unsigned)(s3)) & 31u))) | 1u))))) & 1u) {
    cs = csmix(cs, (unsigned)(helper1(u5, ((unsigned)(u4) >> ((unsigned)((((unsigned)((~((unsigned)(3573647395u) | 0u))) & 1u) ? (unsigned)(4190471408u) : (unsigned)((-((unsigned)(3760752706u) | 0u))))) & 31u)))));
    cs = csmix(cs, (unsigned)(st8.f0));
    { unsigned g10 = 0u;
      while (g10 < 4u) {
        unsigned i9 = g10;
        cs = csmix(cs, i9);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(1759934287u) * (unsigned)(st7.f2))) | (unsigned)(((unsigned)(i9) ^ (unsigned)(2560331773u))))) | (unsigned)(((unsigned)(u5) <= ((unsigned)(((unsigned)(arr6[((unsigned)(u5) & 7u)]) * (unsigned)(arr6[((unsigned)(3853981953u) & 7u)]))) ^ cs))))) % ((unsigned)(((unsigned)(u5) + (unsigned)(((unsigned)(((unsigned)(u4) % ((unsigned)(i9) | 1u))) ^ (unsigned)(((unsigned)(828569205u) << ((unsigned)((unsigned)(s3)) & 31u))))))) | 1u))));
        g10++;
      }
    }
    { unsigned g12 = 0u;
      while (g12 < 2u) {
        unsigned i11 = g12;
        cs = csmix(cs, i11);
        cs = csmix(cs, (unsigned)((unsigned)(s3)));
        g12++;
      }
    }
    cs = csmix(cs, (unsigned)(((unsigned)(u4) & (unsigned)(3882503472u))));
    { unsigned g14 = 0u;
      while (g14 < 8u) {
        unsigned i13 = g14;
        cs = csmix(cs, i13);
        cs = csmix(cs, (unsigned)(u4));
        cs = csmix(cs, (unsigned)(((unsigned)(helper2((~((unsigned)(((unsigned)(3209750947u) * (unsigned)(i13))) | 0u)), ((unsigned)(((unsigned)(arr6[((unsigned)(i13) & 7u)]) + (unsigned)(arr6[((unsigned)(u5) & 7u)]))) & (unsigned)((((unsigned)(2000211306u) & 1u) ? (unsigned)(u5) : (unsigned)(1003988890u)))))) <= ((unsigned)(2059691309u) ^ cs))));
        cs = csmix(cs, (unsigned)(((unsigned)(helper2(u4, st8.f1)) ^ (unsigned)(((unsigned)(helper1(((unsigned)((unsigned)(s3)) - (unsigned)(48678104u)), ((unsigned)((unsigned)(s3)) ^ (unsigned)(714171349u)))) % ((unsigned)(((unsigned)(u5) % ((unsigned)(((unsigned)((unsigned)(s3)) ^ (unsigned)(((unsigned)((unsigned)(s3)) ^ cs)))) | 1u))) | 1u))))));
        g14++;
      }
    }
  } else {
    for (unsigned g16 = 0u; g16 < 12u; g16++) {
      unsigned i15 = g16;
      cs = csmix(cs, i15);
      cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s3)) % ((unsigned)(((unsigned)(arr6[((unsigned)(u4) & 7u)]) | (unsigned)(((unsigned)(((unsigned)(st7.f2) - (unsigned)((unsigned)(s3)))) % ((unsigned)(((unsigned)(247191013u) * (unsigned)(u4))) | 1u))))) | 1u))));
      i15 = (unsigned)(u4) & 0xffffffffu;
      u4 = (unsigned)(u5) & 0xffffffffu;
      arr6[((unsigned)(i15) & 7u)] = (unsigned)(((unsigned)(((unsigned)(arr6[((unsigned)(u5) & 7u)]) ^ (unsigned)((~((unsigned)(3159834990u) | 0u))))) << ((unsigned)(arr6[((unsigned)(u5) & 7u)]) & 31u)));
      cs = csmix(cs, (unsigned)(((unsigned)(3379399073u) & (unsigned)(((unsigned)(((unsigned)(((unsigned)(1898962442u) != ((unsigned)(arr6[((unsigned)(u5) & 7u)]) ^ cs))) | (unsigned)(((unsigned)(1011898761u) * (unsigned)(u4))))) << ((unsigned)(((unsigned)(((unsigned)(arr6[((unsigned)(i15) & 7u)]) * (unsigned)(arr6[((unsigned)(3559830295u) & 7u)]))) != ((unsigned)(2278240483u) ^ cs))) & 31u))))));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(helper2(((unsigned)(st7.f0) | (unsigned)(1331696763u)), ((unsigned)(st7.f2) ^ (unsigned)(785300334u)))) % ((unsigned)(((unsigned)(u4) >> ((unsigned)(i15) & 31u))) | 1u))) == ((unsigned)(st8.f1) ^ cs))));
    }
    cs = csmix(cs, (unsigned)(u4));
    cs = csmix(cs, (unsigned)(2721838283u));
  }
  for (unsigned g18 = 0u; g18 < 12u; g18++) {
    unsigned i17 = g18;
    cs = csmix(cs, i17);
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(arr6[((unsigned)(u4) & 7u)]) + (unsigned)((((unsigned)(st8.f1) & 1u) ? (unsigned)(u4) : (unsigned)(2452366135u))))) & (unsigned)(arr6[((unsigned)(1416610270u) & 7u)]))) / ((unsigned)(i17) | 1u))));
    { unsigned g20 = 0u;
      while (g20 < 6u) {
        unsigned i19 = g20;
        cs = csmix(cs, i19);
        g20++;
      }
    }
    if ((unsigned)(((unsigned)(i17) * (unsigned)(((unsigned)(arr6[((unsigned)(879602745u) & 7u)]) ^ (unsigned)((((unsigned)(((unsigned)(st8.f0) & (unsigned)(954796694u))) & 1u) ? (unsigned)(((unsigned)(st7.f1) * (unsigned)(arr6[((unsigned)(u5) & 7u)]))) : (unsigned)(3583926416u))))))) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((((unsigned)(527151001u) & 1u) ? (unsigned)(u4) : (unsigned)(((unsigned)(u4) ^ cs)))) / ((unsigned)(((unsigned)(u4) - (unsigned)(2005014288u))) | 1u))) * (unsigned)(((unsigned)((((unsigned)(st8.f1) & 1u) ? (unsigned)(1136240483u) : (unsigned)(1477729400u))) % ((unsigned)(st7.f0) | 1u))))) << ((unsigned)(helper2(2644772378u, st7.f0)) & 31u))));
      cs = csmix(cs, (unsigned)(((unsigned)(helper1(((unsigned)(3931107478u) - (unsigned)(((unsigned)(3515910149u) - (unsigned)(arr6[((unsigned)(3962904905u) & 7u)])))), 2327408441u)) << ((unsigned)(((unsigned)((((unsigned)(st7.f0) & 1u) ? (unsigned)(((unsigned)(1821750113u) / ((unsigned)(1254049911u) | 1u))) : (unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)(2167108017u) | 1u))))) << ((unsigned)(helper2(st8.f2, helper1(221790453u, i17))) & 31u))) & 31u))));
      st7.f2 = (unsigned)(((unsigned)((-((unsigned)(arr6[((unsigned)(u5) & 7u)]) | 0u))) ^ (unsigned)(((unsigned)(st8.f1) ^ (unsigned)(((unsigned)(helper1(st7.f1, 2955784934u)) * (unsigned)(((unsigned)(1060679755u) | (unsigned)(arr6[((unsigned)(u4) & 7u)])))))))));
    }
    if ((unsigned)(helper1(((unsigned)(st7.f2) & (unsigned)(2886756027u)), ((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) + (unsigned)(arr6[((unsigned)(u4) & 7u)]))) + (unsigned)((unsigned)(s3)))) * (unsigned)(u4)))) & 1u) {
      cs = csmix(cs, (unsigned)(arr6[((unsigned)(3246550393u) & 7u)]));
      cs = csmix(cs, (unsigned)(helper1(((unsigned)(((unsigned)((unsigned)(s3)) - (unsigned)(((unsigned)(u5) <= ((unsigned)((unsigned)(s3)) ^ cs))))) / ((unsigned)(((unsigned)((~((unsigned)(803634705u) | 0u))) | (unsigned)(((unsigned)(3033070109u) + (unsigned)(arr6[((unsigned)(i17) & 7u)]))))) | 1u)), ((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)(arr6[((unsigned)(3975011294u) & 7u)]) | 1u))) - (unsigned)(666391141u))) | (unsigned)((-((unsigned)(arr6[((unsigned)(u5) & 7u)]) | 0u)))))));
      cs = csmix(cs, (unsigned)(((unsigned)(2768638364u) + (unsigned)(((unsigned)((~((unsigned)(((unsigned)(2416925748u) % ((unsigned)(987605452u) | 1u))) | 0u))) - (unsigned)(1174079704u))))));
      cs = csmix(cs, (unsigned)((~((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) & (unsigned)((-((unsigned)(u4) | 0u))))) ^ (unsigned)(st8.f1))) | 0u))));
    }
  }
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr6[k]);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  cs = csmix(cs, st8.f0);
  cs = csmix(cs, st8.f1);
  cs = csmix(cs, st8.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
