/*
 * volatile fuzz seed 16558 reduction (O1/O2): barrel-shift fusion recorded a
 * hidden `LSL #7` on the OR's src2 in ir->barrel_shifts[] (side-table, keyed
 * by orig_index) and NOPed the SHL; ssa:var_to_param_forward then substituted
 * the single-def VAR's constant (#3) into that annotated src2.  An immediate
 * cannot be barrel-shifted, so codegen silently dropped the shift and
 * `(u6 << 7) | (u6 & s2)` collapsed from 385 to 3.
 * Fixed by blocking VAR->use forwarding into any barrel-shift-annotated
 * instruction in ssa_opt_var_to_param_forward (source/opt/ssa/scalar/cprop.c).
 * Ground truth (tcc -O0, all levels agree after fix): checksum=42f25408.
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
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s2 = (char)(442978609u & 0xff);
  unsigned u3 = 4026948395u;
  unsigned u4 = 686696752u;
  unsigned u5 = 4050428074u;
  unsigned u6 = 1912218721u;
  unsigned u7 = 2178992381u;
  unsigned u8 = 1177103879u;
  volatile unsigned vv9 = 702781133u;
  volatile unsigned vv10 = 4167698068u;
  cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)(helper1(((unsigned)(1223807585u) >> ((unsigned)(1872046338u) & 31u)), 3661176946u)) | 0u))) % ((unsigned)(1428952318u) | 1u))));
  cs = csmix(cs, (unsigned)(((unsigned)(helper1(((unsigned)(u4) / ((unsigned)(helper1(3060100119u, u8)) | 1u)), ((unsigned)(((unsigned)(3729267955u) * (unsigned)((unsigned)(s2)))) + (unsigned)(((unsigned)(2099715918u) | (unsigned)(1413267191u)))))) ^ (unsigned)((-((unsigned)(helper1(((unsigned)(u7) * (unsigned)(3626911326u)), ((unsigned)(u3) != ((unsigned)(15087610u) ^ cs)))) | 0u))))));
  u6 = (unsigned)(((unsigned)(u5) / ((unsigned)(u8) | 1u))) & 0xffffffffu;
  for (unsigned g12 = 0u; g12 < 3u; g12++) {
    unsigned i11 = g12;
    cs = csmix(cs, i11);
    cs = csmix(cs, (unsigned)(1544872618u));
    { unsigned g14 = 0u;
      while (g14 < 4u) {
        unsigned i13 = g14;
        cs = csmix(cs, i13);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(helper1((~((unsigned)((unsigned)(s2)) | 0u)), ((unsigned)(u7) * (unsigned)(1767382181u)))) % ((unsigned)(((unsigned)(2625132895u) & (unsigned)(u6))) | 1u))) & (unsigned)(((unsigned)(helper1((unsigned)(s2), 1462164176u)) >> ((unsigned)(((unsigned)(((unsigned)(u5) - (unsigned)(2739350645u))) >= ((unsigned)(((unsigned)(2477767278u) - (unsigned)((unsigned)(s2)))) ^ cs))) & 31u))))));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u6) << ((unsigned)(u8) & 31u))) | (unsigned)(((unsigned)(u6) & (unsigned)((unsigned)(s2)))))) % ((unsigned)(((unsigned)((((unsigned)(u3) & 1u) ? (unsigned)(2444794533u) : (unsigned)(3916340012u))) + (unsigned)(((unsigned)((unsigned)(s2)) % ((unsigned)(2949418865u) | 1u))))) | 1u))) & (unsigned)(1351162556u))));
        g14++;
      }
    }
  }
  cs = csmix(cs, (unsigned)((unsigned)(s2)));
  if ((unsigned)(((unsigned)((~((unsigned)((~((unsigned)((((unsigned)(662311632u) & 1u) ? (unsigned)(u7) : (unsigned)((unsigned)(s2)))) | 0u))) | 0u))) % ((unsigned)((-((unsigned)((-((unsigned)((((unsigned)(4281228932u) & 1u) ? (unsigned)(893081600u) : (unsigned)(u3))) | 0u))) | 0u))) | 1u))) & 1u) {
  }
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, vv9);
  cs = csmix(cs, vv10);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  printf("checksum=%08x\n", cs);
  return 0;
}
