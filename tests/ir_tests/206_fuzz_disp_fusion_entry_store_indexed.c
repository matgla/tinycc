/* Regression for seed 806: disp_fusion rewrote a later struct-field store
 * into STORE_INDEXED through Addr[StackLoc] + #imm.  entry_store_prop missed
 * that overwrite and forwarded the stale entry initializer for the field. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)((-((unsigned)(pa) | 0u))) >> ((unsigned)(((unsigned)(((unsigned)(pb) << ((unsigned)(405949269u) & 31u))) / ((unsigned)((-((unsigned)(pa) | 0u))) | 1u))) & 31u))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)((-((unsigned)(220938907u) | 0u))) ^ lr;
}
static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(1221736488u) - (unsigned)(((unsigned)((-((unsigned)(4281017552u) | 0u))) ^ (unsigned)(((unsigned)(1048039549u) & (unsigned)(3792990399u))))))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s4 = (char)(70158794u & 0xff);
  char s5 = (char)(2123458335u & 0xff);
  unsigned u6 = 4096537787u;
  unsigned u7 = 3595528999u;
  unsigned u8 = 1324128657u;
  unsigned u9 = 4176613975u;
  unsigned arr10[8] = { 2058031608u, 2049453767u, 3090127593u, 2058904464u, 2293690814u, 1538007302u, 839561433u, 4109109770u };
  unsigned arr11[8] = { 4258876033u, 2658963948u, 3994798954u, 3518464776u, 3903427843u, 4224767271u, 3048695191u, 1798369436u };
  struct S st12 = { 1371112811u, 3565427112u, 1892095003u };
  if ((unsigned)(u8) & 1u) {
    cs = csmix(cs, (unsigned)(((unsigned)(u6) * (unsigned)(((unsigned)(helper1(((unsigned)(402321968u) - (unsigned)(1068965203u)), (unsigned)(s4))) ^ (unsigned)(((unsigned)(u7) / ((unsigned)(st12.f1) | 1u))))))));
    { unsigned g14 = 0u;
      while (g14 < 4u) {
        unsigned i13 = g14;
        cs = csmix(cs, i13);
        cs = csmix(cs, (unsigned)(arr11[((unsigned)(i13) & 7u)]));
        g14++;
      }
    }
    for (unsigned g16 = 0u; g16 < 8u; g16++) {
      unsigned i15 = g16;
      cs = csmix(cs, i15);
    }
    st12.f2 = (unsigned)(u7);
    if ((unsigned)(arr10[((unsigned)(3299466682u) & 7u)]) & 1u) {
      cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(226483673u) + (unsigned)(((unsigned)(u9) % ((unsigned)(1529308387u) | 1u))))) & 1u) ? (unsigned)(((unsigned)(u7) >> ((unsigned)(1288450611u) & 31u))) : (unsigned)(((unsigned)((-((unsigned)(u8) | 0u))) + (unsigned)(u8))))));
      cs = csmix(cs, (unsigned)(((unsigned)(3414038116u) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(st12.f0) % ((unsigned)(arr10[((unsigned)(u6) & 7u)]) | 1u))) & (unsigned)(u9))) >> ((unsigned)(((unsigned)(st12.f2) * (unsigned)(u8))) & 31u))) & 31u))));
      cs = csmix(cs, (unsigned)((((unsigned)((~((unsigned)(((unsigned)(((unsigned)(u9) & (unsigned)(3353340182u))) | (unsigned)(((unsigned)(u6) / ((unsigned)(u7) | 1u))))) | 0u))) & 1u) ? (unsigned)(((unsigned)(u6) - (unsigned)(3186306314u))) : (unsigned)(((unsigned)(((unsigned)(st12.f1) * (unsigned)(((unsigned)(st12.f1) ^ cs)))) / ((unsigned)(u7) | 1u))))));
      cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(u6) | 0u))) & (unsigned)(u7))));
    }
    u8 = (unsigned)(((unsigned)((unsigned)(s4)) + (unsigned)(((unsigned)(2139137487u) / ((unsigned)(((unsigned)(((unsigned)(u6) * (unsigned)(u9))) > ((unsigned)((~((unsigned)(arr10[((unsigned)(u6) & 7u)]) | 0u))) ^ cs))) | 1u))))) & 0xffffffffu;
    { unsigned g18 = 0u;
      while (g18 < 5u) {
        unsigned i17 = g18;
        cs = csmix(cs, i17);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(arr11[((unsigned)(u9) & 7u)]) | (unsigned)(((unsigned)(807305474u) * (unsigned)((-((unsigned)(arr11[((unsigned)(1359191717u) & 7u)]) | 0u))))))) / ((unsigned)(3790498266u) | 1u))));
        cs = csmix(cs, (unsigned)(596631656u));
        cs = csmix(cs, (unsigned)(helper2(((unsigned)(((unsigned)(((unsigned)(u9) | (unsigned)(arr10[((unsigned)(u9) & 7u)]))) ^ (unsigned)((-((unsigned)(1345410543u) | 0u))))) / ((unsigned)(2429833031u) | 1u)), st12.f2)));
        g18++;
      }
    }
    { unsigned g20 = 0u;
      while (g20 < 11u) {
        unsigned i19 = g20;
        cs = csmix(cs, i19);
        cs = csmix(cs, (unsigned)(helper2((-((unsigned)(((unsigned)(220414113u) ^ (unsigned)((~((unsigned)(4037040588u) | 0u))))) | 0u)), ((unsigned)(((unsigned)(((unsigned)(2955146141u) * (unsigned)(2239072099u))) - (unsigned)(880729714u))) * (unsigned)(((unsigned)(614978539u) / ((unsigned)(u6) | 1u)))))));
        g20++;
      }
    }
  }
  for (unsigned g22 = 0u; g22 < 10u; g22++) {
    unsigned i21 = g22;
    cs = csmix(cs, i21);
  }
  cs = csmix(cs, (unsigned)(((unsigned)(st12.f2) & (unsigned)(((unsigned)((~((unsigned)(((unsigned)(2775479217u) % ((unsigned)((unsigned)(s5)) | 1u))) | 0u))) - (unsigned)(((unsigned)(u9) - (unsigned)(helper2(2496637020u, 2174733178u)))))))));
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr10[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr11[k]);
  cs = csmix(cs, st12.f0);
  cs = csmix(cs, st12.f1);
  cs = csmix(cs, st12.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
