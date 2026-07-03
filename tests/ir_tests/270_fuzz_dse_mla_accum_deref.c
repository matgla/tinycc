/* Fuzz regression (struct_byval seed 11651, combo seed 11651; O1/O2 miscompile):
 * two DSE-family passes treated a by-value struct's spilled fields as dead
 * because the only read was through an MLA accumulator:
 *   StackLoc[-8] <-- P0;  T6 <-- Addr[StackLoc[-8]];  T <-- Ta MLA Tb + T6***DEREF***
 * (1) tcc_ir_opt_dse's write-only addr-TMP scan (ir/opt_dce.c) checked only
 *     src1/src2 uses, so T6 looked write-only and the spill stores + Addr def
 *     were NOP'd, leaving the MLA reading undefined memory.
 * (2) tcc_ir_opt_dead_lea_store_elim's operand walk had the same src1/src2
 *     blindness and killed the spill store the same way.
 * Fix: treat the MLA accumulator (4th operand) as a use in both scans. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)((~((unsigned)(((unsigned)((-((unsigned)(1062749631u) | 0u))) <= ((unsigned)(((unsigned)(pb) - (unsigned)(249012051u))) ^ lr))) | 0u))) ^ lr;
}
struct SB1 { unsigned char a; };
struct SB4 { unsigned a; };
struct SB5 { unsigned a; unsigned char b; };
struct SB8 { unsigned a; unsigned b; };
union UB { unsigned w; unsigned char b; };
static struct SB1 sbh2(struct SB8 p, unsigned x)
{
  struct SB1 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffu };
  r.a = (unsigned)(((unsigned)((((unsigned)(363993060u) & 1u) ? (unsigned)(((unsigned)(p.b) * (unsigned)(x))) : (unsigned)(p.a))) + (unsigned)(((unsigned)(((unsigned)(x) << ((unsigned)(((unsigned)(x) ^ x)) & 31u))) * (unsigned)(((unsigned)(x) | (unsigned)(3774950606u))))))) & 0xffu;
  return r;
}
static struct SB1 sbh3(struct SB8 p, unsigned x)
{
  struct SB1 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffu };
  return r;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  short s4 = (short)(1109438112u & 0xffff);
  long s5 = (long)(561989678u & 0xffffffff);
  unsigned u6 = 639754459u;
  unsigned u7 = 2552236936u;
  unsigned arr8[8] = { 2131909559u, 1254228196u, 838819187u, 195968001u, 2996523193u, 4206402744u, 3986925906u, 2662247771u };
  struct S st9 = { 3490662044u, 793968674u, 685315226u };
  struct S st10 = { 708452264u, 2909141502u, 3857364788u };
  if ((unsigned)(u7) & 1u) {
    { unsigned g12 = 0u;
      while (g12 < 6u) {
        unsigned i11 = g12;
        cs = csmix(cs, i11);
        cs = csmix(cs, (unsigned)(((unsigned)(2804715900u) <= ((unsigned)(arr8[((unsigned)(1125179338u) & 7u)]) ^ cs))));
        { struct SB8 sba13 = { (unsigned)(1530714348u) & 0xffffffffu, (unsigned)(((unsigned)((-((unsigned)(2238245109u) | 0u))) << ((unsigned)(helper1(u7, i11)) & 31u))) & 0xffffffffu };
          struct SB1 sbt14 = sbh3(sba13, (unsigned)(((unsigned)(u6) * (unsigned)(((unsigned)(((unsigned)(2509001262u) / ((unsigned)(((unsigned)(u6) - (unsigned)(((unsigned)(u6) ^ cs)))) | 1u))) + (unsigned)(((unsigned)(((unsigned)(2209318298u) ^ (unsigned)(i11))) + (unsigned)(3619053070u))))))));
          cs = csmix(cs, sbt14.a);
        }
      }
    }
    { union UB ub15; ub15.w = (unsigned)(((unsigned)(((unsigned)(((unsigned)(arr8[((unsigned)(u6) & 7u)]) - (unsigned)((unsigned)(s4)))) | (unsigned)(u6))) * (unsigned)(((unsigned)(((unsigned)(3877883862u) & (unsigned)(arr8[((unsigned)(u6) & 7u)]))) ^ (unsigned)(((unsigned)(u7) - (unsigned)(757140828u))))))); cs = csmix(cs, ub15.w); }
    { union UB ub16; ub16.w = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) <= ((unsigned)(1190733649u) ^ cs))) & (unsigned)(arr8[((unsigned)(u6) & 7u)]))) >> ((unsigned)(((unsigned)(arr8[((unsigned)(4270887936u) & 7u)]) + (unsigned)(((unsigned)(703352502u) - (unsigned)(arr8[((unsigned)(u7) & 7u)]))))) & 31u))); cs = csmix(cs, ub16.w); }
    { struct SB8 sba17 = { (unsigned)(((unsigned)(261761994u) / ((unsigned)(st9.f1) | 1u))) & 0xffffffffu, (unsigned)((unsigned)(s5)) & 0xffffffffu };
      struct SB1 sbt18 = sbh3(sba17, (unsigned)(((unsigned)(arr8[((unsigned)(u6) & 7u)]) + (unsigned)(2835729132u))));
      cs = csmix(cs, sbt18.a);
    }
    cs = csmix(cs, (unsigned)((-((unsigned)(((unsigned)(helper1(((unsigned)(u6) <= ((unsigned)(1729587979u) ^ cs)), ((unsigned)(u6) ^ (unsigned)(3879489390u)))) >> ((unsigned)((((unsigned)(arr8[((unsigned)(2150963119u) & 7u)]) & 1u) ? (unsigned)(((unsigned)(st10.f1) ^ (unsigned)(773514104u))) : (unsigned)(((unsigned)((unsigned)(s4)) / ((unsigned)(u7) | 1u))))) & 31u))) | 0u))));
  }
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((unsigned)(s5)) * (unsigned)((((unsigned)(((unsigned)((unsigned)(s4)) * (unsigned)(arr8[((unsigned)(910895476u) & 7u)]))) & 1u) ? (unsigned)(((unsigned)((unsigned)(s5)) % ((unsigned)(u7) | 1u))) : (unsigned)((unsigned)(s4)))))) & (unsigned)(((unsigned)(u7) & (unsigned)((-((unsigned)(arr8[((unsigned)(1210949044u) & 7u)]) | 0u))))))));
  cs = csmix(cs, (unsigned)((((unsigned)((((unsigned)(arr8[((unsigned)(u7) & 7u)]) & 1u) ? (unsigned)(((unsigned)(u7) % ((unsigned)(((unsigned)(arr8[((unsigned)(1179750921u) & 7u)]) & (unsigned)(arr8[((unsigned)(3169144596u) & 7u)]))) | 1u))) : (unsigned)(((unsigned)(((unsigned)(u6) * (unsigned)(1524847004u))) ^ (unsigned)(u6))))) & 1u) ? (unsigned)((-((unsigned)((unsigned)(s4)) | 0u))) : (unsigned)(((unsigned)(u6) ^ (unsigned)(arr8[((unsigned)(u6) & 7u)]))))));
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  { struct SB8 sba19 = { 1u, 2u };
    struct SB1 sbt20 = sbh2(sba19, cs);
    cs = csmix(cs, sbt20.a); }
  { struct SB8 sba21 = { 19088744u, 19088745u };
    struct SB1 sbt22 = sbh3(sba21, cs);
    cs = csmix(cs, sbt22.a); }
  printf("checksum=%08x\n", cs);
  return 0;
}
