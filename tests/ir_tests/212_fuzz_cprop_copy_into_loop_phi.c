/* Regression for differential-fuzz seed 2698: wrong-code at -O2 only.
 *
 * Root cause: ssa_gen_cprop_assign (SSA copy propagation) folded a copy
 * `T_dest <- T_src` whose dest was a PHI operand on a loop back-edge (the
 * loop-carried `cs` value).  Replacing the phi operand with T_src directly and
 * dropping the copy reintroduces the out-of-SSA lost-copy problem: T_src stays
 * live past the phi edge and its slot is overwritten before the parallel-copy
 * phi resolution, corrupting the loop-carried value.  (Exposed only after an
 * unrelated, sound redundant_var_assign DSE reshaped the IR so the copy became
 * propagatable; -fno-dead-store-elim merely hid the trigger.)
 *
 * Fix: cprop_assign must not propagate a copy whose dest feeds a phi operand;
 * the resolving copy is left in place (DCE still removes genuinely dead ones).
 * Ground truth (gcc -m32 -funsigned-char): checksum=157ae9b8 (== tcc -O0/-O1).
 * Buggy -O2 produced a817fbb8.
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
  lr = (unsigned)(1590721590u);
  lr = (unsigned)(3501170363u);
  return (unsigned)(1467210099u) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  char s2 = (char)(1976992943u & 0xff);
  unsigned u3 = 4143460632u;
  unsigned u4 = 1662366621u;
  struct S st5 = { 4005284882u, 2772618132u, 1938930400u };

  { unsigned g7 = 0u;
    while (g7 < 6u) {
      unsigned i6 = g7;
      cs = csmix(cs, i6);
      for (unsigned g9 = 0u; g9 < 2u; g9++) {
        unsigned i8 = g9;
        cs = csmix(cs, i8);
        st5.f2 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(st5.f1) % ((unsigned)(((unsigned)(st5.f0) > ((unsigned)(35409632u) ^ cs))) | 1u))) >> ((unsigned)(u4) & 31u))) * (unsigned)(i8)));
        cs = csmix(cs, (unsigned)(((unsigned)(u4) ^ (unsigned)(helper1((~((unsigned)(((unsigned)(543574967u) >> ((unsigned)(i8) & 31u))) | 0u)), i6)))));
        i6 = (unsigned)(((unsigned)(st5.f1) - (unsigned)((~((unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(((unsigned)(1668911977u) - (unsigned)(3702001358u))) & 31u))) | 0u))))) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(((unsigned)(i8) / ((unsigned)(((unsigned)((-((unsigned)(((unsigned)(st5.f2) & (unsigned)(2593997688u))) | 0u))) * (unsigned)(((unsigned)(((unsigned)(i6) - (unsigned)(1924281802u))) - (unsigned)(st5.f2))))) | 1u))));
        st5.f0 = (unsigned)(((unsigned)(helper1(((unsigned)(((unsigned)(u4) % ((unsigned)((unsigned)(s2)) | 1u))) >> ((unsigned)((~((unsigned)(3539581696u) | 0u))) & 31u)), ((unsigned)(((unsigned)(1989370529u) * (unsigned)(st5.f2))) ^ (unsigned)(((unsigned)(st5.f0) + (unsigned)(627147366u)))))) << ((unsigned)((((unsigned)(st5.f0) & 1u) ? (unsigned)(535011871u) : (unsigned)(3079771224u))) & 31u)));
      }
      u4 = (unsigned)(i6) & 0xffffffffu;
      st5.f2 = (unsigned)(((unsigned)(i6) < ((unsigned)((unsigned)(s2)) ^ cs)));
      for (unsigned g11 = 0u; g11 < 9u; g11++) {
        unsigned i10 = g11;
        cs = csmix(cs, i10);
        st5.f1 = (unsigned)(((unsigned)(((unsigned)(st5.f1) << ((unsigned)(helper1((((unsigned)(u4) & 1u) ? (unsigned)(st5.f2) : (unsigned)(i6)), ((unsigned)(u4) > ((unsigned)(st5.f0) ^ cs)))) & 31u))) & (unsigned)(((unsigned)((-((unsigned)(((unsigned)(u3) % ((unsigned)(3065034805u) | 1u))) | 0u))) | (unsigned)(((unsigned)(st5.f1) & (unsigned)(((unsigned)(2496369616u) * (unsigned)(st5.f1)))))))));
      }
      cs = csmix(cs, (unsigned)(((unsigned)(i6) - (unsigned)(helper1((-((unsigned)(u3) | 0u)), st5.f2)))));
      g7++;
    }
  }
  if ((unsigned)(((unsigned)(4102113271u) & (unsigned)(1095710701u))) & 1u) {
    u3 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) % ((unsigned)(helper1(u4, (unsigned)(s2))) | 1u))) - (unsigned)(((unsigned)(((unsigned)(u3) + (unsigned)(1368273083u))) & (unsigned)(((unsigned)(st5.f1) * (unsigned)(u3))))))) << ((unsigned)(136777276u) & 31u))) & 0xffffffffu;
    u3 = (unsigned)(((unsigned)(((unsigned)(3142431158u) / ((unsigned)(2887415043u) | 1u))) + (unsigned)(((unsigned)(((unsigned)(u3) / ((unsigned)(((unsigned)(u3) ^ cs)) | 1u))) ^ (unsigned)(((unsigned)(((unsigned)(2675216160u) * (unsigned)(u3))) % ((unsigned)(((unsigned)(2365060173u) << ((unsigned)((unsigned)(s2)) & 31u))) | 1u))))))) & 0xffffffffu;
    cs = csmix(cs, (unsigned)((~((unsigned)(((unsigned)((~((unsigned)((((unsigned)((unsigned)(s2)) & 1u) ? (unsigned)(u3) : (unsigned)(st5.f0))) | 0u))) & (unsigned)(((unsigned)(((unsigned)(st5.f2) << ((unsigned)(440354889u) & 31u))) <= ((unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(u3))) ^ cs))))) | 0u))));
    u4 = (unsigned)(((unsigned)(u4) + (unsigned)(st5.f1))) & 0xffffffffu;
    u3 = (unsigned)(((unsigned)(2564216260u) << ((unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(((unsigned)(((unsigned)(3523360677u) + (unsigned)(u4))) - (unsigned)(((unsigned)(3378690058u) ^ (unsigned)(3929119652u))))) : (unsigned)(((unsigned)((((unsigned)(u3) & 1u) ? (unsigned)(1988010103u) : (unsigned)(2595131026u))) * (unsigned)((((unsigned)(u3) & 1u) ? (unsigned)(u3) : (unsigned)(st5.f2))))))) & 31u))) & 0xffffffffu;
    if ((unsigned)(helper1(3149014152u, (unsigned)(s2))) & 1u) {
      st5.f1 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(1903915767u) & (unsigned)(u4))) - (unsigned)(((unsigned)(1670752140u) > ((unsigned)(st5.f0) ^ cs))))) | (unsigned)(((unsigned)(u4) & (unsigned)(((unsigned)(u4) & (unsigned)((unsigned)(s2)))))))) - (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) | (unsigned)(3913968285u))) - (unsigned)(st5.f1)))));
      u3 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(2712135088u) << ((unsigned)(st5.f0) & 31u))) & (unsigned)(((unsigned)(848928383u) / ((unsigned)(u3) | 1u))))) * (unsigned)((unsigned)(s2)))) + (unsigned)(((unsigned)(((unsigned)(((unsigned)(st5.f2) / ((unsigned)(st5.f0) | 1u))) ^ (unsigned)(((unsigned)(u4) == ((unsigned)(u3) ^ cs))))) >> ((unsigned)((-((unsigned)(((unsigned)(u3) / ((unsigned)((unsigned)(s2)) | 1u))) | 0u))) & 31u))))) & 0xffffffffu;
      u3 = (unsigned)(helper1(3666404167u, ((unsigned)(1975115836u) * (unsigned)(1311310750u)))) & 0xffffffffu;
    }
  }
  for (unsigned g13 = 0u; g13 < 10u; g13++) {
    unsigned i12 = g13;
    cs = csmix(cs, i12);
    st5.f2 = (unsigned)(((unsigned)(((unsigned)(u3) - (unsigned)(((unsigned)(233968521u) | (unsigned)(((unsigned)(4128230827u) & (unsigned)(2261308614u))))))) | (unsigned)(((unsigned)(((unsigned)(((unsigned)(st5.f2) + (unsigned)((unsigned)(s2)))) - (unsigned)(4080903397u))) - (unsigned)(4124531007u)))));
    i12 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(st5.f1) - (unsigned)((-((unsigned)(i12) | 0u))))) << ((unsigned)(((unsigned)(((unsigned)(u4) / ((unsigned)(((unsigned)(u4) ^ cs)) | 1u))) % ((unsigned)((unsigned)(s2)) | 1u))) & 31u))) << ((unsigned)(((unsigned)(((unsigned)(((unsigned)(3582757195u) * (unsigned)(i12))) - (unsigned)(helper1(3821621905u, (unsigned)(s2))))) / ((unsigned)(i12) | 1u))) & 31u))) & 0xffffffffu;
  }
  u4 = (unsigned)((unsigned)(s2)) & 0xffffffffu;
  for (unsigned g15 = 0u; g15 < 4u; g15++) {
    unsigned i14 = g15;
    cs = csmix(cs, i14);
    { unsigned g17 = 0u;
      while (g17 < 11u) {
        unsigned i16 = g17;
        cs = csmix(cs, i16);
        cs = csmix(cs, (unsigned)(491941173u));
        st5.f0 = (unsigned)(2208843311u);
        i16 = (unsigned)(((unsigned)(4056105524u) << ((unsigned)(i16) & 31u))) & 0xffffffffu;
        i14 = (unsigned)(((unsigned)(helper1(u3, ((unsigned)(((unsigned)(u3) >> ((unsigned)(418737203u) & 31u))) * (unsigned)(st5.f2)))) & (unsigned)(u4))) & 0xffffffffu;
        g17++;
      }
    }
    u3 = (unsigned)(1522124972u) & 0xffffffffu;
    u4 = (unsigned)(((unsigned)(((unsigned)(2300161773u) < ((unsigned)(((unsigned)((((unsigned)(u3) & 1u) ? (unsigned)(2567970456u) : (unsigned)(605141359u))) | (unsigned)(((unsigned)(st5.f0) / ((unsigned)((unsigned)(s2)) | 1u))))) ^ cs))) * (unsigned)((unsigned)(s2)))) & 0xffffffffu;
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(2540351223u) / ((unsigned)((unsigned)(s2)) | 1u))) + (unsigned)(115603461u))) - (unsigned)(2554581753u))) ^ (unsigned)((unsigned)(s2)))));
    cs = csmix(cs, (unsigned)((~((unsigned)((~((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(1539211852u))) * (unsigned)(1026266348u))) | 0u))) | 0u))));
  }
  for (unsigned g19 = 0u; g19 < 1u; g19++) {
    unsigned i18 = g19;
    cs = csmix(cs, i18);
    i18 = (unsigned)(((unsigned)((-((unsigned)(((unsigned)(((unsigned)(st5.f1) - (unsigned)(i18))) / ((unsigned)((-((unsigned)(st5.f1) | 0u))) | 1u))) | 0u))) | (unsigned)(i18))) & 0xffffffffu;
  }

  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, st5.f0);
  cs = csmix(cs, st5.f1);
  cs = csmix(cs, st5.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
