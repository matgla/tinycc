#include <stdio.h>

/*
 * Fuzz struct_byval seed 6105 reduction (O1/O2): in sbh1's `p.a + p.b` arm
 * (by-value struct fields at SP-relative slots), the real-run codegen ran
 * out of scratch registers where the dry run had not (so no scratch save
 * area was reserved), and get_scratch_reg_with_save fell back to PUSH in an
 * FP-omitted frame: `push {r0}; ldr r0, [sp, #8]` — the SP-relative offset
 * was computed for the pre-push SP, reading 4 bytes below p.a.
 * Fix: bias every SP-relative frame access (fp_adjust_local_offset and the
 * scratch save-area LDR/STR sites) by 4 bytes per scratch PUSH currently
 * active in the real run, so a push window can no longer skew frame reads.
 * Expected checksum (gcc -O2 arm-none-eabi + tcc -O0/-Os): 52b169aa.
 */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct SB1 { unsigned char a; };
struct SB4 { unsigned a; };
struct SB5 { unsigned a; unsigned char b; };
struct SB8 { unsigned a; unsigned b; };
union UB { unsigned w; unsigned char b; };
static struct SB1 sbh1(struct SB8 p, unsigned x)
{
  struct SB1 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffu };
  r.a = (unsigned)(((unsigned)(p.a) >> ((unsigned)((((unsigned)(x) & 1u) ? (unsigned)((~((unsigned)(p.b) | 0u))) : (unsigned)(((unsigned)(p.a) + (unsigned)(p.b))))) & 31u))) & 0xffu;
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
  long s2 = (long)(1604498154u & 0xffffffff);
  int s3 = (int)(430245816u & 0xffffffff);
  unsigned u4 = 498953156u;
  unsigned u5 = 2636751711u;
  unsigned u6 = 2601838499u;
  unsigned u7 = 1376987875u;
  unsigned u8 = 232652266u;
  unsigned u9 = 3094303121u;
  unsigned arr10[8] = { 3600399106u, 4134203428u, 4194447185u, 1645705583u, 1535751434u, 2936151218u, 2499992786u, 3152020498u };
  unsigned arr11[8] = { 3849630906u, 3586782890u, 3297031665u, 1405981475u, 1034717908u, 3458871662u, 3427095025u, 304844470u };
  struct S st12 = { 2110935441u, 2145556735u, 3419096170u };
  if ((unsigned)(st12.f0) & 1u) {
    for (unsigned g14 = 0u; g14 < 4u; g14++) {
      unsigned i13 = g14;
      cs = csmix(cs, i13);
      { union UB ub15; ub15.w = (unsigned)(u9); cs = csmix(cs, ub15.w); }
    }
    for (unsigned g17 = 0u; g17 < 12u; g17++) {
      unsigned i16 = g17;
      cs = csmix(cs, i16);
      cs = csmix(cs, (unsigned)(arr11[((unsigned)(1517824458u) & 7u)]));
      { union UB ub18; ub18.w = (unsigned)(((unsigned)((~((unsigned)(((unsigned)(2833394604u) > ((unsigned)((unsigned)(s3)) ^ cs))) | 0u))) | (unsigned)(u7))); cs = csmix(cs, ub18.w); }
      { union UB ub19; ub19.w = (unsigned)(arr11[((unsigned)(3420876253u) & 7u)]); cs = csmix(cs, ub19.w); }
      { union UB ub20; ub20.w = (unsigned)(((unsigned)(st12.f0) == ((unsigned)(arr11[((unsigned)(u8) & 7u)]) ^ cs))); cs = csmix(cs, ub20.w); }
      { struct SB8 sba21 = { (unsigned)((-((unsigned)(((unsigned)(arr11[((unsigned)(u8) & 7u)]) / ((unsigned)(2897360735u) | 1u))) | 0u))) & 0xffffffffu, (unsigned)(i16) & 0xffffffffu };
        struct SB1 sbt22 = sbh1(sba21, (unsigned)(((unsigned)(((unsigned)(((unsigned)(arr10[((unsigned)(u6) & 7u)]) & (unsigned)(((unsigned)(arr10[((unsigned)(u6) & 7u)]) ^ cs)))) >= ((unsigned)(u6) ^ cs))) >> ((unsigned)(((unsigned)(((unsigned)((~((unsigned)(u4) | 0u))) & (unsigned)(((unsigned)(u4) * (unsigned)(u6))))) - (unsigned)(((unsigned)(u4) | (unsigned)(((unsigned)(u4) ^ (unsigned)((unsigned)(s2)))))))) & 31u))));
        cs = csmix(cs, sbt22.a);
      }
      { union UB ub23; ub23.w = (unsigned)(i16); cs = csmix(cs, ub23.w); }
    }
    { struct SB8 sba24 = { (unsigned)(((unsigned)(((unsigned)(arr10[((unsigned)(u6) & 7u)]) + (unsigned)(arr10[((unsigned)(u9) & 7u)]))) >> ((unsigned)(((unsigned)(u6) * (unsigned)((unsigned)(s3)))) & 31u))) & 0xffffffffu, (unsigned)(st12.f2) & 0xffffffffu };
      struct SB1 sbt25 = sbh1(sba24, (unsigned)(((unsigned)(((unsigned)(2202021010u) % ((unsigned)(((unsigned)(u5) - (unsigned)(st12.f1))) | 1u))) ^ (unsigned)(u8))));
      cs = csmix(cs, sbt25.a);
    }
  }
  if ((unsigned)(((unsigned)((((unsigned)(arr10[((unsigned)(4160996959u) & 7u)]) & 1u) ? (unsigned)(u4) : (unsigned)((-((unsigned)(1271821855u) | 0u))))) >> ((unsigned)(((unsigned)(((unsigned)(u7) | (unsigned)(3368565741u))) & (unsigned)(((unsigned)(((unsigned)(arr10[((unsigned)(u6) & 7u)]) ^ (unsigned)(st12.f2))) / ((unsigned)(((unsigned)(arr10[((unsigned)(u6) & 7u)]) + (unsigned)((unsigned)(s2)))) | 1u))))) & 31u))) & 1u) {
    { struct SB8 sba26 = { (unsigned)((~((unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(850203603u))) | 0u))) & 0xffffffffu, (unsigned)((~((unsigned)(2948530897u) | 0u))) & 0xffffffffu };
      struct SB1 sbt27 = sbh1(sba26, (unsigned)(((unsigned)(((unsigned)(u4) ^ (unsigned)(952436950u))) & (unsigned)((((unsigned)(3288257197u) & 1u) ? (unsigned)((-((unsigned)(((unsigned)(1299647295u) & (unsigned)(u4))) | 0u))) : (unsigned)(arr11[((unsigned)(u4) & 7u)]))))));
      cs = csmix(cs, sbt27.a);
    }
    cs = csmix(cs, (unsigned)((unsigned)(s2)));
  }
  for (unsigned g29 = 0u; g29 < 9u; g29++) {
    unsigned i28 = g29;
    cs = csmix(cs, i28);
    { struct SB8 sba30 = { (unsigned)(arr10[((unsigned)(3865815134u) & 7u)]) & 0xffffffffu, (unsigned)(((unsigned)(2975594741u) >> ((unsigned)((~((unsigned)(st12.f1) | 0u))) & 31u))) & 0xffffffffu };
      struct SB1 sbt31 = sbh1(sba30, (unsigned)((~((unsigned)(arr10[((unsigned)(u4) & 7u)]) | 0u))));
      cs = csmix(cs, sbt31.a);
    }
    cs = csmix(cs, (unsigned)(u8));
    { unsigned g33 = 0u;
      while (g33 < 12u) {
        unsigned i32 = g33;
        cs = csmix(cs, i32);
        { struct SB8 sba34 = { (unsigned)((-((unsigned)((unsigned)(s3)) | 0u))) & 0xffffffffu, (unsigned)(2464351619u) & 0xffffffffu };
          struct SB1 sbt35 = sbh1(sba34, (unsigned)(u9));
          cs = csmix(cs, sbt35.a);
        }
        cs = csmix(cs, (unsigned)(159253768u));
        { union UB ub36; ub36.w = (unsigned)((-((unsigned)(((unsigned)(u5) - (unsigned)(arr11[((unsigned)(u7) & 7u)]))) | 0u))); cs = csmix(cs, ub36.w); }
        g33++;
      }
    }
  }
  if ((unsigned)(((unsigned)(((unsigned)(u8) % ((unsigned)((((unsigned)(st12.f1) & 1u) ? (unsigned)(3451053359u) : (unsigned)(((unsigned)(u7) >> ((unsigned)(537436575u) & 31u))))) | 1u))) + (unsigned)(126981882u))) & 1u) {
  }
  if ((unsigned)((-((unsigned)(((unsigned)(((unsigned)(u7) << ((unsigned)(((unsigned)((unsigned)(s2)) | (unsigned)(((unsigned)((unsigned)(s2)) ^ cs)))) & 31u))) + (unsigned)((((unsigned)(st12.f2) & 1u) ? (unsigned)(((unsigned)(arr10[((unsigned)(3588395976u) & 7u)]) / ((unsigned)((unsigned)(s3)) | 1u))) : (unsigned)(u9))))) | 0u))) & 1u) {
  }
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr10[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr11[k]);
  cs = csmix(cs, st12.f0);
  cs = csmix(cs, st12.f1);
  cs = csmix(cs, st12.f2);
  { struct SB8 sba37 = { 1u, 2u };
    struct SB1 sbt38 = sbh1(sba37, cs);
    cs = csmix(cs, sbt38.a); }
  printf("checksum=%08x\n", cs);
  return 0;
}
