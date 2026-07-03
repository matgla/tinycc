/* Regression: NOT a tcc bug -- this pins tcc's CORRECT output for a program on
 * which the differential fuzzer's gcc oracle is itself wrong.
 *
 * Source: gen_c.py --profile bitfield --seed 1486.  Reported as vs-gcc
 * "divergent" in fuzz_triage_all_1000_5000.md, but the divergence is a
 * gcc -O2 MISCOMPILE, not a tcc defect:
 *
 *   correct  = checksum=dcc35a7a  <- tcc O0/O1/O2/Os, gcc -O0/-O1,
 *                                     clang -O0/-O2, and an exact 32-bit
 *                                     C-semantics reference model all agree
 *   wrong    = checksum=b8eb5045  <- ONLY arm-none-eabi-gcc -O2/-O3/-Os
 *                                     (also host gcc 16.1.1 -O2; two independent
 *                                     gcc versions), localized to the
 *                                     helper1(1u, cs) call site in main.
 *
 * The program is UB-free: clang's real-UB sanitizer is clean, all divisors are
 * `| 1u`, all shift counts `& 31u`, all array indices `& 7u` into [8], every
 * local is initialized, and -fwrapv/-fno-strict-aliasing do not change gcc's
 * wrong result.  So "passing" this seed means tcc must keep emitting dcc35a7a
 * at every -O level; this test guards against a future tcc change regressing it.
 * See memory: bitfield-1486-gcc-o2-false-positive.
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
  lr = (unsigned)(((unsigned)(pa) + (unsigned)(2170103671u)));
  lr = (unsigned)(lr);
  lr = (unsigned)(((unsigned)(pa) ^ (unsigned)((((unsigned)(((unsigned)(1591420043u) - (unsigned)(pb))) & 1u) ? (unsigned)(((unsigned)(450256323u) ^ (unsigned)(1402591702u))) : (unsigned)(168206180u)))));
  if ((unsigned)(((unsigned)(((unsigned)(pb) / ((unsigned)(447454083u) | 1u))) * (unsigned)(lr))) & 1u) lr += (unsigned)(pb);
  if ((unsigned)((((unsigned)(((unsigned)(pb) >> ((unsigned)(4031838544u) & 31u))) & 1u) ? (unsigned)(pa) : (unsigned)(((unsigned)(862129824u) ^ (unsigned)(pb))))) & 1u) lr += (unsigned)(lr);
  return (unsigned)(pb) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(((unsigned)(((unsigned)(266199230u) << ((unsigned)(3898932827u) & 31u))) - (unsigned)((((unsigned)(1012794546u) & 1u) ? (unsigned)(pa) : (unsigned)(3132687816u))))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(2226548946u) / ((unsigned)(pb) | 1u))) >> ((unsigned)(3857755331u) & 31u)));
  lr = (unsigned)((~((unsigned)(lr) | 0u)));
  if ((unsigned)(((unsigned)(((unsigned)(lr) ^ (unsigned)(3510969157u))) * (unsigned)(((unsigned)(pa) - (unsigned)(lr))))) & 1u) lr += (unsigned)(((unsigned)(464211921u) > ((unsigned)(pb) ^ lr)));
  lr = (unsigned)(pb);
  return (unsigned)(((unsigned)((-((unsigned)(((unsigned)(lr) % ((unsigned)(1373399917u) | 1u))) | 0u))) % ((unsigned)(((unsigned)(2076176889u) / ((unsigned)(((unsigned)(1788951503u) % ((unsigned)(pa) | 1u))) | 1u))) | 1u))) ^ lr;
}

static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)(((unsigned)(((unsigned)(1401581598u) & (unsigned)(pb))) << ((unsigned)(((unsigned)(447574553u) / ((unsigned)(1932625153u) | 1u))) & 31u))) < ((unsigned)((((unsigned)(((unsigned)(3977891575u) ^ (unsigned)(3262698102u))) & 1u) ? (unsigned)((~((unsigned)(686525409u) | 0u))) : (unsigned)(((unsigned)(1792213612u) & (unsigned)(2551422570u))))) ^ lr)));
  lr = (unsigned)(((unsigned)(((unsigned)(((unsigned)(pa) + (unsigned)(2857583892u))) & (unsigned)(((unsigned)(pa) >> ((unsigned)(pb) & 31u))))) % ((unsigned)(((unsigned)(lr) & (unsigned)(3022310170u))) | 1u)));
  return (unsigned)(pb) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

struct BF {
  unsigned b0 : 8;
  unsigned b1 : 1;
  unsigned b2 : 2;
};

#pragma pack(push, 1)
struct BFP {
  unsigned b0 : 5;
  unsigned b1 : 4;
  unsigned b2 : 11;
  unsigned b3 : 4;
} __attribute__((packed));
#pragma pack(pop)

int main(void)
{
  unsigned cs = 0x12345678u;
  long s4 = (long)(1026744710u & 0xffffffff);
  int s5 = (int)(885014010u & 0xffffffff);
  unsigned u6 = 2689141333u;
  unsigned u7 = 3303677935u;
  unsigned u8 = 3150409694u;
  unsigned u9 = 2523915598u;
  unsigned arr10[8] = { 2780099274u, 3468858776u, 2779735268u, 4094356366u, 2025283576u, 1229269038u, 564453088u, 877852883u };
  unsigned arr11[8] = { 935704511u, 486158863u, 1346136272u, 3960564353u, 2239498372u, 3416187467u, 3386489379u, 2840277157u };
  struct BF bf12 = { 0u, 0u, 0u };

  u9 = (unsigned)((unsigned)(s4)) & 0xffffffffu;
  u6 = (unsigned)((((unsigned)(arr10[((unsigned)(2505094822u) & 7u)]) & 1u) ? (unsigned)((unsigned)(s4)) : (unsigned)(u6))) & 0xffffffffu;
  for (unsigned g14 = 0u; g14 < 7u; g14++) {
    unsigned i13 = g14;
    cs = csmix(cs, i13);
    u9 = (unsigned)(((unsigned)(((unsigned)(((unsigned)((~((unsigned)(arr11[((unsigned)(2266088073u) & 7u)]) | 0u))) ^ (unsigned)(((unsigned)(i13) % ((unsigned)(839863841u) | 1u))))) * (unsigned)(u6))) << ((unsigned)(((unsigned)(((unsigned)((-((unsigned)(u7) | 0u))) ^ (unsigned)((((unsigned)((unsigned)(s5)) & 1u) ? (unsigned)(u7) : (unsigned)(u8))))) / ((unsigned)(((unsigned)(((unsigned)(2503236673u) / ((unsigned)(3529741934u) | 1u))) + (unsigned)(((unsigned)(u7) / ((unsigned)((unsigned)(s5)) | 1u))))) | 1u))) & 31u))) & 0xffffffffu;
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((~((unsigned)(u6) | 0u))) < ((unsigned)(((unsigned)((~((unsigned)(u7) | 0u))) % ((unsigned)(((unsigned)(u7) - (unsigned)(2440177590u))) | 1u))) ^ cs))) >> ((unsigned)(4291152536u) & 31u))));
    u9 = (unsigned)(u6) & 0xffffffffu;
  }

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
  cs = csmix(cs, bf12.b0);
  cs = csmix(cs, bf12.b1);
  cs = csmix(cs, bf12.b2);
  printf("checksum=%08x\n", cs);
  return 0;
}
