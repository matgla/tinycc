/* volatile fuzz seed 64026 (O1 internal compiler error, "missing FUNCPARAMVAL
 * for call_id=N"): the identical-block loop re-roller (ir/opt_reroll.c) matched
 * a PHASE-SHIFTED window over a run of calls.  The innermost loop body holds
 *   cs = csmix(cs, vv12);  cs = csmix(cs, vv12);  cs = csmix(cs, vv12);  ...
 * i.e. `PARAM0; PARAM1; CALL` repeated.  Because the matcher compares blocks by
 * opcode/operand shape only, the window `CALL; PARAM0; PARAM1` matches just as
 * well as the natural `PARAM0; PARAM1; CALL`.  Re-rolling the shifted window put
 * the period boundary between a call's FUNCPARAMVAL markers and its own
 * FUNCCALLVAL, NOP-ing the last call's params while leaving its CALL standing
 * just past the run -- the backend callsite scan then aborted with
 * "missing FUNCPARAMVAL for call_id=20".  Fixed by requiring the canonical body
 * to be call-balanced (body_calls_balanced): every FUNCCALLVAL's params must lie
 * in the same body before it and no FUNCPARAMVAL may dangle past the boundary,
 * which forces the boundary onto a real call-group edge (natural alignment).
 *
 * Reference (arm-none-eabi-gcc -O2): checksum=67527ae2, matching tcc -O0/-O2. */
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
  if ((unsigned)((~((unsigned)(((unsigned)(pb) | (unsigned)(255233952u))) | 0u))) & 1u) lr += (unsigned)(76484648u);
  lr = (unsigned)(pb);
  return (unsigned)(((unsigned)(pa) / ((unsigned)(pb) | 1u))) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(pa);
  lr = (unsigned)(3589883255u);
  return (unsigned)(((unsigned)(108341958u) ^ (unsigned)(((unsigned)(((unsigned)(3667775691u) % ((unsigned)(lr) | 1u))) + (unsigned)(1523996112u))))) ^ lr;
}

static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)((~((unsigned)((~((unsigned)(1373884012u) | 0u))) | 0u))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(lr) != ((unsigned)(((unsigned)(lr) ^ lr)) ^ lr))) / ((unsigned)(lr) | 1u)));
  lr = (unsigned)(((unsigned)(pb) << ((unsigned)(((unsigned)(helper2(306437994u, 3863829684u)) == ((unsigned)((((unsigned)(pb) & 1u) ? (unsigned)(417355562u) : (unsigned)(1622462493u))) ^ lr))) & 31u)));
  lr = (unsigned)(((unsigned)(3033434179u) * (unsigned)(((unsigned)(((unsigned)(pa) >> ((unsigned)(3283623890u) & 31u))) & (unsigned)(pb)))));
  lr = (unsigned)(1849000114u);
  return (unsigned)(((unsigned)(((unsigned)((-((unsigned)(3105299237u) | 0u))) + (unsigned)(((unsigned)(3059778045u) | (unsigned)(3558975240u))))) >> ((unsigned)(((unsigned)(helper2(1524793457u, 1102027024u)) << ((unsigned)(((unsigned)(pb) >= ((unsigned)(((unsigned)(pb) ^ lr)) ^ lr))) & 31u))) & 31u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  char s4 = (char)(39962773u & 0xff);
  long s5 = (long)(1359109588u & 0xffffffff);
  int s6 = (int)(1872451407u & 0xffffffff);
  unsigned u7 = 2593177108u;
  unsigned u8 = 961326210u;
  unsigned u9 = 140185649u;
  unsigned u10 = 1933618952u;
  volatile unsigned vv11 = 2048839005u;
  volatile unsigned vv12 = 593626131u;

  { unsigned g14 = 0u;
    while (g14 < 11u) {
      unsigned i13 = g14;
      cs = csmix(cs, i13);
      { unsigned g16 = 0u;
        while (g16 < 1u) {
          unsigned i15 = g16;
          cs = csmix(cs, i15);
          vv12 = (unsigned)(((unsigned)(u10) ^ (unsigned)(u9)));
          vv12 = (unsigned)(((unsigned)(((unsigned)(i13) % ((unsigned)(u9) | 1u))) / ((unsigned)((unsigned)(s5)) | 1u)));
          i15 = (unsigned)(i13) & 0xffffffffu;
          u9 = (unsigned)(((unsigned)(((unsigned)((~((unsigned)(((unsigned)(629550400u) << ((unsigned)(992883592u) & 31u))) | 0u))) << ((unsigned)(2934866509u) & 31u))) < ((unsigned)(((unsigned)((unsigned)(s4)) | (unsigned)((unsigned)(s6)))) ^ cs))) & 0xffffffffu;
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(532690437u) + (unsigned)(i15))) != ((unsigned)(((unsigned)(u10) / ((unsigned)(3158691876u) | 1u))) ^ cs))) + (unsigned)((-((unsigned)(((unsigned)(u10) ^ (unsigned)(u7))) | 0u))))) > ((unsigned)(((unsigned)(((unsigned)((~((unsigned)((unsigned)(s4)) | 0u))) * (unsigned)((-((unsigned)((unsigned)(s5)) | 0u))))) * (unsigned)(((unsigned)(((unsigned)(3229320235u) / ((unsigned)((unsigned)(s6)) | 1u))) % ((unsigned)(((unsigned)(i13) % ((unsigned)(((unsigned)(i13) ^ cs)) | 1u))) | 1u))))) ^ cs))));
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(707554611u) / ((unsigned)(u8) | 1u))) + (unsigned)(((unsigned)(2808571992u) | (unsigned)(u7))))) - (unsigned)(1066522307u))) - (unsigned)(((unsigned)(((unsigned)(helper2(3390914276u, 1923883233u)) & (unsigned)(((unsigned)((unsigned)(s4)) | (unsigned)(u8))))) - (unsigned)((-((unsigned)(((unsigned)(1638822050u) ^ (unsigned)(3539210301u))) | 0u))))))));
          g16++;
        }
      }
      cs = csmix(cs, (unsigned)(((unsigned)(2869952186u) >> ((unsigned)(((unsigned)(((unsigned)(3054473495u) << ((unsigned)(1071193574u) & 31u))) + (unsigned)(i13))) & 31u))));
      i13 = (unsigned)((unsigned)(s5)) & 0xffffffffu;
      cs = csmix(cs, vv12);
      g14++;
    }
  }
  cs = csmix(cs, vv11);
  cs = csmix(cs, (unsigned)(((unsigned)(helper3(u10, 3364973046u)) % ((unsigned)(helper3(((unsigned)(((unsigned)((unsigned)(s4)) >> ((unsigned)(1496991228u) & 31u))) % ((unsigned)(((unsigned)(u8) >> ((unsigned)(u10) & 31u))) | 1u)), 827067730u)) | 1u))));
  cs = csmix(cs, vv12);
  { unsigned g18 = 0u;
    while (g18 < 10u) {
      unsigned i17 = g18;
      cs = csmix(cs, i17);
      u7 = (unsigned)(u7) & 0xffffffffu;
      u7 = (unsigned)(u7) & 0xffffffffu;
      if ((unsigned)(((unsigned)(2921773780u) - (unsigned)(u9))) & 1u) {
        vv12 = (unsigned)(((unsigned)(((unsigned)((~((unsigned)((-((unsigned)(i17) | 0u))) | 0u))) + (unsigned)((unsigned)(s4)))) >> ((unsigned)(((unsigned)(2161923515u) | (unsigned)(((unsigned)(((unsigned)(u10) < ((unsigned)(u9) ^ cs))) >> ((unsigned)(((unsigned)(2772568355u) * (unsigned)((unsigned)(s5)))) & 31u))))) & 31u)));
        cs = csmix(cs, vv12);
      } else {
        cs = csmix(cs, (unsigned)(u9));
        u9 = (unsigned)(3458786600u) & 0xffffffffu;
      }
      cs = csmix(cs, (unsigned)(((unsigned)(65807866u) % ((unsigned)(((unsigned)(u8) / ((unsigned)(u7) | 1u))) | 1u))));
      u9 = (unsigned)((unsigned)(s6)) & 0xffffffffu;
      for (unsigned g20 = 0u; g20 < 3u; g20++) {
        unsigned i19 = g20;
        cs = csmix(cs, i19);
        cs = csmix(cs, vv12);
        cs = csmix(cs, vv12);
        cs = csmix(cs, vv12);
        cs = csmix(cs, vv12);
        cs = csmix(cs, (unsigned)(3491778562u));
      }
      g18++;
    }
  }
  cs = csmix(cs, (unsigned)(373005310u));

  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, u10);
  cs = csmix(cs, vv11);
  cs = csmix(cs, vv12);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  cs = csmix(cs, (unsigned)s6);
  printf("checksum=%08x\n", cs);
  return 0;
}
