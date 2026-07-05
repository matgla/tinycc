/* Regression test for a loop-carried value clobbered by the scratch picker.
 *
 * Verbatim fuzz repro (gen_c.py seed=244).  Loop-carried values live across a
 * loop body via the back-edge, but the interval-derived live-regs bitmap models
 * each value as one [def,last-use] range and leaves the loop-header prefix
 * uncovered, so the scratch-register picker reused the register inside the loop
 * and clobbered the carried value (-O2 HardFault).  Fixed by loop-liveness
 * completion over a real CFG dataflow (ra_refine_live_regs_accurate,
 * ir/regalloc.c).  Expected checksum (gcc -O0/-O1/-O2 all agree): ce53d2eb.
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
  if ((unsigned)((((unsigned)(((unsigned)(4156688943u) << ((unsigned)(499794113u) & 31u))) & 1u) ? (unsigned)(3567543948u) : (unsigned)(lr))) & 1u) lr += (unsigned)(pb);
  lr = (unsigned)(pa);
  lr = (unsigned)(((unsigned)(pa) + (unsigned)(1630909929u)));
  lr = (unsigned)(((unsigned)(((unsigned)(145694363u) / ((unsigned)(207166839u) | 1u))) ^ (unsigned)(pb)));
  if ((unsigned)(((unsigned)(lr) - (unsigned)(((unsigned)(826925892u) << ((unsigned)(lr) & 31u))))) & 1u) lr += (unsigned)(pb);
  return (unsigned)(1366548986u) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)(((unsigned)(1150506092u) <= ((unsigned)(((unsigned)(lr) * (unsigned)(2307127124u))) ^ lr))) - (unsigned)(pa)));
  lr = (unsigned)(4186682686u);
  lr = (unsigned)(((unsigned)(((unsigned)((-((unsigned)(3426736310u) | 0u))) + (unsigned)(2265647840u))) | (unsigned)(pb)));
  lr = (unsigned)(pb);
  if ((unsigned)(((unsigned)((((unsigned)(190099325u) & 1u) ? (unsigned)(3599992223u) : (unsigned)(1013057225u))) + (unsigned)(((unsigned)(pa) << ((unsigned)(lr) & 31u))))) & 1u) lr += (unsigned)(1016372499u);
  return (unsigned)(((unsigned)((~((unsigned)(((unsigned)(pb) % ((unsigned)(lr) | 1u))) | 0u))) | (unsigned)(((unsigned)(446909555u) * (unsigned)(pb))))) ^ lr;
}

static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(pb);
  lr = (unsigned)((-((unsigned)(((unsigned)(((unsigned)(193825971u) * (unsigned)(736205326u))) - (unsigned)(((unsigned)(pa) ^ (unsigned)(pb))))) | 0u)));
  if ((unsigned)(pb) & 1u) lr += (unsigned)(helper2(((unsigned)(664724830u) - (unsigned)(pa)), ((unsigned)(1243414760u) * (unsigned)(1235706782u))));
  return (unsigned)((-((unsigned)(pb) | 0u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  short s4 = (short)(1473033926u & 0xffff);
  char s5 = (char)(421113725u & 0xff);
  int s6 = (int)(288860313u & 0xffffffff);
  unsigned u7 = 2929235060u;
  unsigned u8 = 1604926888u;
  unsigned u9 = 2012695995u;
  unsigned u10 = 3146539104u;
  unsigned u11 = 3805893888u;
  struct S st12 = { 4075333170u, 3725495817u, 1365083859u };

  u10 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u8) >= ((unsigned)(((unsigned)(u8) ^ cs)) ^ cs))) | (unsigned)(((unsigned)(st12.f2) | (unsigned)(2883464662u))))) >= ((unsigned)(st12.f1) ^ cs))) % ((unsigned)(st12.f0) | 1u))) & 0xffffffffu;
  u10 = (unsigned)(((unsigned)(((unsigned)((-((unsigned)((-((unsigned)((unsigned)(s5)) | 0u))) | 0u))) % ((unsigned)(((unsigned)(2591932674u) / ((unsigned)(2694458890u) | 1u))) | 1u))) & (unsigned)(3876640516u))) & 0xffffffffu;
  { unsigned g14 = 0u;
    while (g14 < 7u) {
      unsigned i13 = g14;
      cs = csmix(cs, i13);
      if ((unsigned)(((unsigned)((-((unsigned)(((unsigned)(helper1(1290308682u, st12.f0)) % ((unsigned)(((unsigned)((unsigned)(s4)) != ((unsigned)(st12.f0) ^ cs))) | 1u))) | 0u))) + (unsigned)(((unsigned)(((unsigned)(helper1(4248564424u, 2366590552u)) / ((unsigned)(st12.f0) | 1u))) * (unsigned)(((unsigned)(u11) * (unsigned)((((unsigned)(900739701u) & 1u) ? (unsigned)(u9) : (unsigned)(u11))))))))) & 1u) {
        cs = csmix(cs, (unsigned)((-((unsigned)(((unsigned)(u9) - (unsigned)((-((unsigned)(((unsigned)(st12.f0) << ((unsigned)(927649216u) & 31u))) | 0u))))) | 0u))));
        cs = csmix(cs, (unsigned)(helper1(((unsigned)(((unsigned)((unsigned)(s4)) % ((unsigned)((-((unsigned)(586255013u) | 0u))) | 1u))) >= ((unsigned)((-((unsigned)(((unsigned)(u11) * (unsigned)(u10))) | 0u))) ^ cs)), u11)));
        i13 = (unsigned)(((unsigned)((~((unsigned)((~((unsigned)(u11) | 0u))) | 0u))) % ((unsigned)(((unsigned)(((unsigned)(((unsigned)(2893830294u) | (unsigned)(i13))) * (unsigned)(627060177u))) / ((unsigned)((unsigned)(s5)) | 1u))) | 1u))) & 0xffffffffu;
        u8 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(918852153u) != ((unsigned)(1435437827u) ^ cs))) & (unsigned)(((unsigned)(2309643186u) >> ((unsigned)(st12.f1) & 31u))))) | (unsigned)(3817474092u))) & 0xffffffffu;
      } else {
        cs = csmix(cs, (unsigned)(helper2((-((unsigned)(2334617814u) | 0u)), ((unsigned)(u9) | (unsigned)((~((unsigned)(u9) | 0u)))))));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((~((unsigned)(i13) | 0u))) | (unsigned)((~((unsigned)(((unsigned)(u10) / ((unsigned)(u7) | 1u))) | 0u))))) & (unsigned)(((unsigned)(((unsigned)((-((unsigned)(i13) | 0u))) * (unsigned)(((unsigned)(u7) / ((unsigned)(u11) | 1u))))) != ((unsigned)(((unsigned)(((unsigned)(u10) - (unsigned)(u8))) | (unsigned)(helper2(i13, 3823949253u)))) ^ cs))))));
      }
      if ((unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) / ((unsigned)(i13) | 1u))) + (unsigned)(2557191171u))) & (unsigned)(1666522847u))) ^ (unsigned)(((unsigned)(u9) <= ((unsigned)(((unsigned)(2272054492u) <= ((unsigned)(((unsigned)(968020830u) + (unsigned)(1107354u))) ^ cs))) ^ cs))))) & 1u) {
        cs = csmix(cs, (unsigned)(helper3(((unsigned)(((unsigned)(((unsigned)(2742535503u) | (unsigned)(st12.f2))) | (unsigned)(((unsigned)(st12.f2) | (unsigned)((unsigned)(s5)))))) - (unsigned)(((unsigned)(2010811432u) + (unsigned)((~((unsigned)(u8) | 0u)))))), (unsigned)(s6))));
      } else {
        u7 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(st12.f1) <= ((unsigned)(u8) ^ cs))) | (unsigned)(((unsigned)(u8) ^ (unsigned)(st12.f2))))) | (unsigned)(u9))) & (unsigned)((-((unsigned)(((unsigned)(((unsigned)(u7) ^ (unsigned)(st12.f2))) ^ (unsigned)(((unsigned)(1404221428u) - (unsigned)((unsigned)(s6)))))) | 0u))))) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)(((unsigned)(((unsigned)(u9) + (unsigned)((unsigned)(s6)))) * (unsigned)(st12.f2))) | 0u))) | (unsigned)((((unsigned)((unsigned)(s4)) & 1u) ? (unsigned)((unsigned)(s5)) : (unsigned)(((unsigned)(st12.f1) % ((unsigned)((~((unsigned)(i13) | 0u))) | 1u))))))));
        cs = csmix(cs, (unsigned)(u9));
        st12.f0 = (unsigned)(u10);
        u7 = (unsigned)(helper2(((unsigned)(((unsigned)(((unsigned)(u8) & (unsigned)(u7))) > ((unsigned)((~((unsigned)(st12.f2) | 0u))) ^ cs))) / ((unsigned)(((unsigned)(((unsigned)(u10) * (unsigned)(3261097968u))) | (unsigned)(762060380u))) | 1u)), ((unsigned)(helper1(u8, u8)) & (unsigned)(((unsigned)((-((unsigned)(u7) | 0u))) % ((unsigned)(u9) | 1u)))))) & 0xffffffffu;
      }
      cs = csmix(cs, (unsigned)(st12.f0));
      u7 = (unsigned)(((unsigned)(u8) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) - (unsigned)(2256200230u))) << ((unsigned)((((unsigned)(u9) & 1u) ? (unsigned)(i13) : (unsigned)(2098622171u))) & 31u))) - (unsigned)(st12.f2))) & 31u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)(u7) * (unsigned)(((unsigned)(47093188u) >> ((unsigned)(st12.f0) & 31u))))));
      { unsigned g16 = 0u;
        while (g16 < 4u) {
          unsigned i15 = g16;
          cs = csmix(cs, i15);
          st12.f2 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(947487311u) | (unsigned)(((unsigned)(st12.f2) | (unsigned)(u9))))) << ((unsigned)((~((unsigned)(((unsigned)(u10) ^ (unsigned)((unsigned)(s5)))) | 0u))) & 31u))) != ((unsigned)(((unsigned)(st12.f0) << ((unsigned)(((unsigned)((~((unsigned)(1008400238u) | 0u))) & (unsigned)(u7))) & 31u))) ^ cs)));
          g16++;
        }
      }
      g14++;
    }
  }
  st12.f0 = (unsigned)(2773350337u);
  cs = csmix(cs, (unsigned)(((unsigned)(st12.f0) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u11) << ((unsigned)(st12.f2) & 31u))) - (unsigned)((unsigned)(s5)))) | (unsigned)(((unsigned)(st12.f0) * (unsigned)(((unsigned)(3043292983u) * (unsigned)(1826350708u))))))) & 31u))));
  { unsigned g18 = 0u;
    while (g18 < 11u) {
      unsigned i17 = g18;
      cs = csmix(cs, i17);
      { unsigned g20 = 0u;
        while (g20 < 8u) {
          unsigned i19 = g20;
          cs = csmix(cs, i19);
          cs = csmix(cs, (unsigned)((unsigned)(s6)));
          cs = csmix(cs, (unsigned)(((unsigned)(u10) | (unsigned)(((unsigned)((-((unsigned)(((unsigned)(2601304429u) ^ (unsigned)((unsigned)(s4)))) | 0u))) / ((unsigned)(((unsigned)(((unsigned)(st12.f1) & (unsigned)(u7))) + (unsigned)(st12.f0))) | 1u))))));
          u10 = (unsigned)(((unsigned)(1235646853u) < ((unsigned)(((unsigned)(st12.f1) % ((unsigned)(((unsigned)((-((unsigned)(u10) | 0u))) % ((unsigned)((((unsigned)(4015455392u) & 1u) ? (unsigned)(u11) : (unsigned)(u10))) | 1u))) | 1u))) ^ cs))) & 0xffffffffu;
          i19 = (unsigned)(u11) & 0xffffffffu;
          g20++;
        }
      }
      if ((unsigned)(140213096u) & 1u) {
        st12.f0 = (unsigned)(((unsigned)((unsigned)(s4)) % ((unsigned)(i17) | 1u)));
        u8 = (unsigned)(((unsigned)((unsigned)(s5)) % ((unsigned)(((unsigned)(549197135u) / ((unsigned)(((unsigned)(((unsigned)(4019045727u) > ((unsigned)(st12.f1) ^ cs))) & (unsigned)(u10))) | 1u))) | 1u))) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((((unsigned)(((unsigned)(u8) | (unsigned)(i17))) & 1u) ? (unsigned)(((unsigned)(i17) ^ (unsigned)((unsigned)(s5)))) : (unsigned)(((unsigned)(u8) / ((unsigned)(2040040047u) | 1u))))) >> ((unsigned)(((unsigned)(3963607197u) << ((unsigned)(((unsigned)(653116818u) >> ((unsigned)(2356618785u) & 31u))) & 31u))) & 31u))) < ((unsigned)(((unsigned)(((unsigned)((-((unsigned)((unsigned)(s5)) | 0u))) >> ((unsigned)(u10) & 31u))) << ((unsigned)(u9) & 31u))) ^ cs))));
      } else {
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(u10) << ((unsigned)(st12.f0) & 31u))) ^ (unsigned)((unsigned)(s4)))) / ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u8) != ((unsigned)((unsigned)(s4)) ^ cs))) << ((unsigned)(helper3(u8, u7)) & 31u))) % ((unsigned)((unsigned)(s6)) | 1u))) | 1u))));
        cs = csmix(cs, (unsigned)((~((unsigned)(st12.f2) | 0u))));
        cs = csmix(cs, (unsigned)(2457966861u));
        st12.f2 = (unsigned)(helper2((((unsigned)(((unsigned)(((unsigned)(1959764627u) - (unsigned)(i17))) - (unsigned)(helper2(798697879u, (unsigned)(s4))))) & 1u) ? (unsigned)(2069985762u) : (unsigned)(st12.f0)), u10));
        cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)((-((unsigned)(helper2(u9, 4094689284u)) | 0u))) / ((unsigned)(st12.f0) | 1u))) & 1u) ? (unsigned)((-((unsigned)((-((unsigned)((-((unsigned)(u7) | 0u))) | 0u))) | 0u))) : (unsigned)((~((unsigned)(((unsigned)(((unsigned)(u10) - (unsigned)(489340572u))) + (unsigned)((unsigned)(s6)))) | 0u))))));
      }
      for (unsigned g22 = 0u; g22 < 11u; g22++) {
        unsigned i21 = g22;
        cs = csmix(cs, i21);
        cs = csmix(cs, (unsigned)(((unsigned)(u10) & (unsigned)(((unsigned)(((unsigned)(((unsigned)(u8) ^ (unsigned)((unsigned)(s5)))) << ((unsigned)(((unsigned)(i21) % ((unsigned)(u8) | 1u))) & 31u))) * (unsigned)(((unsigned)(((unsigned)(2278223218u) << ((unsigned)(u8) & 31u))) + (unsigned)(((unsigned)(u7) >= ((unsigned)((unsigned)(s5)) ^ cs))))))))));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(3879950840u) * (unsigned)(st12.f2))) >> ((unsigned)(((unsigned)(2432674425u) >= ((unsigned)(i17) ^ cs))) & 31u))) ^ (unsigned)(3547340300u))) % ((unsigned)(3120365575u) | 1u))));
        cs = csmix(cs, (unsigned)(u8));
        u8 = (unsigned)(((unsigned)(((unsigned)((-((unsigned)(((unsigned)(1812337111u) ^ (unsigned)((unsigned)(s5)))) | 0u))) ^ (unsigned)(st12.f0))) ^ (unsigned)(((unsigned)(((unsigned)(3913478367u) / ((unsigned)(((unsigned)(i17) * (unsigned)(u8))) | 1u))) - (unsigned)(3088140881u))))) & 0xffffffffu;
      }
      g18++;
    }
  }

  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, u10);
  cs = csmix(cs, u11);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  cs = csmix(cs, (unsigned)s6);
  cs = csmix(cs, st12.f0);
  cs = csmix(cs, st12.f1);
  cs = csmix(cs, st12.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
