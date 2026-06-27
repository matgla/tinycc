/* Regression: differential-fuzz seed 1454 (-O2 miscompile).
 *
 * Pass: ssa_opt_sccp (ir/opt/ssa_opt_sccp.c), JUMPIF edge evaluation.
 * Root cause: a conditional branch whose taken target equals its fall-through
 *   block (a JUMPIF to the next instruction) has a single CFG successor.  SCCP
 *   derived the fall-through block as "a successor != target_block"; with only
 *   that one successor it left fall_block = -1, and when the branch resolved
 *   "not taken" it added a CFG edge to block -1 -- leaving the real successor
 *   (and the `u5 = 0` reassignment it carries into the merge phi) unreachable.
 *   The phi then dropped that value and folded the array index to u5's entry
 *   value 3, storing to the wrong element.
 * Trigger: redundant_var_assign + DCE delete the only instruction between an
 *   inlined helper's dead `if (lr&1) lr += C` test and its target, collapsing
 *   the branch into the degenerate JUMPIF-to-fall-through shape.
 * Fix: when no distinct fall-through successor exists, fall_block = target_block.
 *
 * UB-free; gcc -m32 -funsigned-char prints checksum=458680e0 at -O0/-O2.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(lr) & 1u) lr += (unsigned)((-((unsigned)(4173887001u) | 0u)));
  lr = (unsigned)(lr);
  lr = (unsigned)(pb);
  return (unsigned)(pa) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)((-((unsigned)(((unsigned)(pa) * (unsigned)(lr))) | 0u))) + (unsigned)(((unsigned)(((unsigned)(3513785869u) | (unsigned)(1490501005u))) << ((unsigned)(((unsigned)(pb) << ((unsigned)(4008053719u) & 31u))) & 31u)))));
  return (unsigned)(lr) ^ lr;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  short s3 = (short)(705526677u & 0xffff);
  int s4 = (int)(933724662u & 0xffffffff);
  unsigned u5 = 3942003363u;
  unsigned u6 = 758880435u;
  unsigned u7 = 376060567u;
  unsigned u8 = 4287571470u;
  unsigned u9 = 3415757831u;
  unsigned u10 = 3609768391u;
  unsigned arr11[8] = { 1581754116u, 2394101820u, 1849028759u, 3443268474u, 2606827072u, 366239643u, 3452365025u, 2820932796u };
  if ((unsigned)(u5) & 1u) {
    arr11[((unsigned)(u10) & 7u)] = (unsigned)(((unsigned)(arr11[((unsigned)(u5) & 7u)]) & (unsigned)(((unsigned)(1696124164u) % ((unsigned)(((unsigned)(1614054316u) ^ (unsigned)(((unsigned)(3761349889u) & (unsigned)(4112562727u))))) | 1u)))));
    u7 = (unsigned)(((unsigned)(u5) | (unsigned)(4220164251u))) & 0xffffffffu;
    if ((unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) % ((unsigned)(((unsigned)(helper2(1267567974u, (unsigned)(s3))) | (unsigned)(((unsigned)(u6) - (unsigned)(u9))))) | 1u))) - (unsigned)(1021393619u))) & 1u) {
      arr11[((unsigned)(3297239061u) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(1381951966u) / ((unsigned)((unsigned)(s3)) | 1u))) - (unsigned)(((unsigned)(u5) % ((unsigned)(u8) | 1u))))) & (unsigned)(((unsigned)(((unsigned)(991141244u) ^ (unsigned)(u10))) | (unsigned)(u10))))) & (unsigned)(((unsigned)((-((unsigned)(helper1(4191172320u, u7)) | 0u))) * (unsigned)(((unsigned)(u8) * (unsigned)(((unsigned)(3646830270u) & (unsigned)(1784161508u)))))))));
      u5 = (unsigned)(((unsigned)(3272064017u) / ((unsigned)(((unsigned)(u6) >> ((unsigned)((((unsigned)(2385025157u) & 1u) ? (unsigned)(((unsigned)(arr11[((unsigned)(u7) & 7u)]) ^ (unsigned)(u8))) : (unsigned)((-((unsigned)(arr11[((unsigned)(u7) & 7u)]) | 0u))))) & 31u))) | 1u))) & 0xffffffffu;
      u5 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) * (unsigned)(u5))) + (unsigned)(((unsigned)(arr11[((unsigned)(3516780014u) & 7u)]) + (unsigned)(2930549122u))))) ^ (unsigned)(3420205976u))) % ((unsigned)(2068657574u) | 1u))) & 0xffffffffu;
      u6 = (unsigned)(((unsigned)((~((unsigned)(2962023217u) | 0u))) & (unsigned)(((unsigned)(3821122726u) - (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) + (unsigned)(u10))) / ((unsigned)(((unsigned)(u5) | (unsigned)((unsigned)(s4)))) | 1u))))))) & 0xffffffffu;
    }
    u10 = (unsigned)(((unsigned)((((unsigned)(((unsigned)(3297129916u) + (unsigned)(((unsigned)((unsigned)(s4)) ^ (unsigned)(arr11[((unsigned)(400225805u) & 7u)]))))) & 1u) ? (unsigned)(arr11[((unsigned)(u6) & 7u)]) : (unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) >= ((unsigned)(u7) ^ cs))) - (unsigned)(((unsigned)((unsigned)(s4)) * (unsigned)(u7))))))) ^ (unsigned)(((unsigned)((((unsigned)(1945153708u) & 1u) ? (unsigned)(((unsigned)(u9) - (unsigned)(arr11[((unsigned)(u9) & 7u)]))) : (unsigned)(((unsigned)(arr11[((unsigned)(u9) & 7u)]) * (unsigned)(u8))))) - (unsigned)(((unsigned)(arr11[((unsigned)(u10) & 7u)]) ^ (unsigned)(arr11[((unsigned)(u6) & 7u)]))))))) & 0xffffffffu;
    arr11[((unsigned)(u5) & 7u)] = (unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)(3202735836u) | 1u))) % ((unsigned)(((unsigned)(12704055u) | (unsigned)((unsigned)(s3)))) | 1u)));
    for (unsigned g13 = 0u; g13 < 4u; g13++) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      cs = csmix(cs, (unsigned)(((unsigned)(3421153611u) & (unsigned)(helper1((~((unsigned)(((unsigned)(arr11[((unsigned)(1483807275u) & 7u)]) / ((unsigned)(u8) | 1u))) | 0u)), (((unsigned)(((unsigned)(2682253260u) - (unsigned)(u9))) & 1u) ? (unsigned)(((unsigned)(arr11[((unsigned)(u7) & 7u)]) & (unsigned)(u10))) : (unsigned)(((unsigned)((unsigned)(s4)) % ((unsigned)(1111857457u) | 1u)))))))));
      cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s3)) < ((unsigned)(((unsigned)(u9) >> ((unsigned)(arr11[((unsigned)(1893038907u) & 7u)]) & 31u))) ^ cs))));
      arr11[((unsigned)(u10) & 7u)] = (unsigned)(((unsigned)(arr11[((unsigned)(i12) & 7u)]) - (unsigned)(((unsigned)(((unsigned)(1543442877u) & (unsigned)(((unsigned)(3180412956u) << ((unsigned)(1608335739u) & 31u))))) << ((unsigned)(((unsigned)(((unsigned)(arr11[((unsigned)(u6) & 7u)]) % ((unsigned)(1317615350u) | 1u))) * (unsigned)(((unsigned)(u6) * (unsigned)(841552286u))))) & 31u)))));
      u10 = (unsigned)(u7) & 0xffffffffu;
      arr11[((unsigned)(4136107824u) & 7u)] = (unsigned)(((unsigned)((unsigned)(s4)) | (unsigned)(u6)));
    }
    { unsigned g15 = 0u;
      while (g15 < 6u) {
        g15++;
      }
    }
    { unsigned g19 = 0u;
      while (g19 < 2u) {
        g19++;
      }
    }
  }
  u5 = (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) - (unsigned)(u6))) - (unsigned)(u9))) & 0xffffffffu;
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(u6) * (unsigned)(((unsigned)((unsigned)(s3)) | (unsigned)(((unsigned)(324578669u) * (unsigned)(u6))))))) << ((unsigned)(helper2((unsigned)(s3), ((unsigned)(((unsigned)(arr11[((unsigned)(3603511482u) & 7u)]) * (unsigned)(u10))) >> ((unsigned)(u6) & 31u)))) & 31u))));
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(helper2(((unsigned)((unsigned)(s4)) >> ((unsigned)(1312435522u) & 31u)), ((unsigned)(u7) % ((unsigned)(769409739u) | 1u)))) | (unsigned)(((unsigned)(((unsigned)(u5) - (unsigned)((unsigned)(s3)))) / ((unsigned)((unsigned)(s4)) | 1u))))) >> ((unsigned)(((unsigned)(1987498061u) - (unsigned)(((unsigned)(((unsigned)(u8) % ((unsigned)(3198883424u) | 1u))) - (unsigned)(((unsigned)(3486661342u) * (unsigned)((unsigned)(s4)))))))) & 31u))));
  cs = csmix(cs, (unsigned)(helper1(((unsigned)(((unsigned)(u9) % ((unsigned)(u10) | 1u))) << ((unsigned)(u5) & 31u)), ((unsigned)(4234050857u) << ((unsigned)(u9) & 31u)))));
  arr11[((unsigned)(u7) & 7u)] = (unsigned)((~((unsigned)((unsigned)(s3)) | 0u)));
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, u10);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr11[k]);
  printf("checksum=%08x\n", cs);
}