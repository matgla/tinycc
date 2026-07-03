#include <stdio.h>

/*
 * Fuzz ptr seed 5759 reduction (O2 HardFault): a literal-pool flush landed
 * INSIDE an ITE block.  tcc_gen_machine_select_mop reserved only the code
 * bytes of the ITE, but the then-arm's load_full_const allocated a NEW pool
 * entry, so the else-arm crossed the 1020-byte threshold in ot() and the
 * pool (with its B.W skip-branch) was emitted in the else-arm's slot: the
 * EQ path fell through into pool data and executed it (wild bus fault).
 * Fix: ot() pre-flushes the pool before an IT/ITE opcode if the worst-case
 * block (code + new pool entries) could hit the threshold, and never
 * flushes while inside an IT block.
 */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(3762768563u);
  if ((unsigned)(((unsigned)((-((unsigned)(417223032u) | 0u))) | (unsigned)(514246771u))) & 1u) lr += (unsigned)(lr);
  return (unsigned)((-((unsigned)(1669260835u) | 0u))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)((-((unsigned)(((unsigned)(484220682u) ^ (unsigned)(lr))) | 0u))) & (unsigned)(((unsigned)(pa) & (unsigned)(2619021163u))))) ^ lr;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  short s3 = (short)(31185974u & 0xffff);
  unsigned u4 = 4115055068u;
  unsigned u5 = 1431129044u;
  unsigned u6 = 2114094315u;
  unsigned u7 = 3636917851u;
  unsigned u8 = 1719970526u;
  unsigned u9 = 653265049u;
  unsigned arr10[8] = { 1624474812u, 3503128519u, 1913935756u, 3984522263u, 3617238270u, 306267720u, 3354816275u, 3307594622u };
  unsigned *p11 = &arr10[((unsigned)(u4) & 7u)];
  unsigned *p12 = &arr10[7u];
  unsigned *p13 = &arr10[((unsigned)(u4) & 7u)];
  if ((unsigned)(arr10[((unsigned)(u4) & 7u)]) & 1u) {
    arr10[((unsigned)(u5) & 7u)] = (unsigned)(((unsigned)(((unsigned)((-((unsigned)(u9) | 0u))) << ((unsigned)(((unsigned)(((unsigned)(3945471139u) / ((unsigned)(u9) | 1u))) + (unsigned)(((unsigned)((unsigned)(s3)) ^ (unsigned)(u5))))) & 31u))) + (unsigned)(((unsigned)(u7) * (unsigned)(2016471384u)))));
    *p13 = (unsigned)(3792151212u);
    cs = csmix(cs, *p13);
    cs = csmix(cs, (unsigned)((((unsigned)(((unsigned)(1114787248u) << ((unsigned)(499138127u) & 31u))) & 1u) ? (unsigned)(3851972995u) : (unsigned)(arr10[((unsigned)(966846521u) & 7u)]))));
    cs = csmix(cs, (unsigned)(((unsigned)(arr10[((unsigned)(2314716422u) & 7u)]) == ((unsigned)(((unsigned)(u8) > ((unsigned)(((unsigned)(((unsigned)(1060724884u) * (unsigned)(3520722114u))) * (unsigned)(((unsigned)(3347177973u) >> ((unsigned)(arr10[((unsigned)(4134773982u) & 7u)]) & 31u))))) ^ cs))) ^ cs))));
    for (unsigned g15 = 0u; g15 < 9u; g15++) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      cs = csmix(cs, (unsigned)(3726623807u));
      *p13 = (unsigned)(((unsigned)(arr10[((unsigned)(2261405896u) & 7u)]) & (unsigned)((unsigned)(s3))));
      cs = csmix(cs, *p11);
      *p11 = (unsigned)(u7);
      cs = csmix(cs, *p13);
      cs = csmix(cs, (unsigned)(2240916218u));
      arr10[((unsigned)(3365475168u) & 7u)] = (unsigned)(u5);
    }
  } else {
    u5 = (unsigned)(((unsigned)((~((unsigned)(4185925909u) | 0u))) + (unsigned)(((unsigned)(2985602767u) >> ((unsigned)(2913548668u) & 31u))))) & 0xffffffffu;
    *p12 = (unsigned)(u4);
    cs = csmix(cs, *p12);
    u6 = (unsigned)((unsigned)(s3)) & 0xffffffffu;
    arr10[((unsigned)(u8) & 7u)] = (unsigned)(1381660145u);
  }
  for (unsigned g17 = 0u; g17 < 3u; g17++) {
    unsigned i16 = g17;
    cs = csmix(cs, i16);
    { unsigned g19 = 0u;
      while (g19 < 3u) {
        unsigned i18 = g19;
        cs = csmix(cs, i18);
        u4 = (unsigned)(((unsigned)(u8) / ((unsigned)(u7) | 1u))) & 0xffffffffu;
        *p13 = (unsigned)((unsigned)(s3));
        cs = csmix(cs, *p13);
        arr10[((unsigned)(689176772u) & 7u)] = (unsigned)((*p13));
        g19++;
      }
    }
    u7 = (unsigned)(arr10[((unsigned)(u7) & 7u)]) & 0xffffffffu;
    arr10[((unsigned)(1903172334u) & 7u)] = (unsigned)(((unsigned)(((unsigned)(arr10[((unsigned)(170951170u) & 7u)]) & (unsigned)((unsigned)(s3)))) / ((unsigned)((~((unsigned)(u5) | 0u))) | 1u)));
    arr10[((unsigned)(u4) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(1125184262u) ^ (unsigned)((unsigned)(s3)))) % ((unsigned)((~((unsigned)(arr10[((unsigned)(3487570623u) & 7u)]) | 0u))) | 1u))) & (unsigned)(((unsigned)(((unsigned)((*p13)) - (unsigned)(u8))) >> ((unsigned)(1707141216u) & 31u))))) | (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) >> ((unsigned)(arr10[((unsigned)(u7) & 7u)]) & 31u))) / ((unsigned)(((unsigned)(603058597u) >> ((unsigned)(u6) & 31u))) | 1u))) | (unsigned)((*p13))))));
    { unsigned g21 = 0u;
      while (g21 < 6u) {
        unsigned i20 = g21;
        cs = csmix(cs, i20);
        u4 = (unsigned)(i20) & 0xffffffffu;
        arr10[((unsigned)(2253921538u) & 7u)] = (unsigned)((unsigned)(s3));
        cs = csmix(cs, (unsigned)(((unsigned)(28876577u) + (unsigned)(helper1(((unsigned)((-((unsigned)(1702706572u) | 0u))) * (unsigned)(((unsigned)(2095160595u) + (unsigned)(u5)))), 1684465951u)))));
        *p11 = (unsigned)((-((unsigned)((*p12)) | 0u)));
        cs = csmix(cs, *p13);
        *p13 = (unsigned)(244864169u);
        cs = csmix(cs, *p11);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((*p13)) << ((unsigned)(((unsigned)(((unsigned)(u4) - (unsigned)(u9))) | (unsigned)(((unsigned)(u8) / ((unsigned)(55757440u) | 1u))))) & 31u))) % ((unsigned)(788737929u) | 1u))));
        arr10[((unsigned)(u6) & 7u)] = (unsigned)(((unsigned)(1650016082u) << ((unsigned)(((unsigned)((-((unsigned)(2447523894u) | 0u))) % ((unsigned)((-((unsigned)(arr10[((unsigned)(3340189745u) & 7u)]) | 0u))) | 1u))) & 31u)));
        g21++;
      }
    }
  }
  cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s3)) >> ((unsigned)(((unsigned)(((unsigned)(u4) << ((unsigned)(269658411u) & 31u))) | (unsigned)(370428068u))) & 31u))));
  for (unsigned g23 = 0u; g23 < 3u; g23++) {
    unsigned i22 = g23;
    cs = csmix(cs, i22);
    cs = csmix(cs, (unsigned)(1968181830u));
    *p12 = (unsigned)((((unsigned)(((unsigned)((-((unsigned)((*p11)) | 0u))) >> ((unsigned)(arr10[((unsigned)(i22) & 7u)]) & 31u))) & 1u) ? (unsigned)(3493057620u) : (unsigned)(u8)));
    cs = csmix(cs, *p12);
    cs = csmix(cs, (unsigned)(arr10[((unsigned)(u4) & 7u)]));
    arr10[((unsigned)(606621756u) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) > ((unsigned)((unsigned)(s3)) ^ cs))) - (unsigned)(((unsigned)(2123292526u) << ((unsigned)(u8) & 31u))))) + (unsigned)(u5))) + (unsigned)(((unsigned)(u9) >> ((unsigned)(((unsigned)(u6) + (unsigned)(((unsigned)(u6) ^ cs)))) & 31u)))));
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((~((unsigned)(1021988837u) | 0u))) << ((unsigned)(arr10[((unsigned)(2762393198u) & 7u)]) & 31u))) & (unsigned)(((unsigned)(757320002u) / ((unsigned)((~((unsigned)(u6) | 0u))) | 1u))))) << ((unsigned)(((unsigned)(((unsigned)(arr10[((unsigned)(3421449680u) & 7u)]) | (unsigned)(((unsigned)(arr10[((unsigned)(u5) & 7u)]) | (unsigned)(arr10[((unsigned)(537095308u) & 7u)]))))) >> ((unsigned)((-((unsigned)((*p12)) | 0u))) & 31u))) & 31u))));
  }
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr10[k]);
  cs = csmix(cs, *p11);
  cs = csmix(cs, *p12);
  cs = csmix(cs, *p13);
  printf("checksum=%08x\n", cs);
  return 0;
}
