/* fuzz struct_byval seed 182993 (O1/O2 divergence).
 * Pass: ssa:var_to_param_forward (ir/opt/ssa_opt_cprop.c).
 * Root cause: forwarding a single-def VAR's constant into a STORE_INDEXED
 * value operand used the immediate's own (narrower) btype for the store
 * width.  u4 = (unsigned)s3 (short) folded to a 16-bit-typed constant; the
 * `st9.f2 = u4` STORE_INDEXED then emitted `strh` (16-bit) instead of `str`,
 * dropping the high 16 bits (0xffff95fd stored as 0x....95fd).
 * Fix: preserve the replaced operand's btype when substituting into a
 * STORE_INDEXED value operand.
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
  return (unsigned)(pb) ^ lr;
}
struct SB1 { unsigned char a; };
struct SB4 { unsigned a; };
struct SB5 { unsigned a; unsigned char b; };
struct SB8 { unsigned a; unsigned b; };
union UB { unsigned w; unsigned char b; };
static struct SB4 sbh2(struct SB4 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
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
  short s3 = (short)(1070372349u & 0xffff);
  unsigned u4 = 3554622350u;
  unsigned u5 = 589675511u;
  unsigned u6 = 3849899987u;
  unsigned u7 = 1109449124u;
  unsigned u8 = 3040234319u;
  struct S st9 = { 2586409042u, 2751080848u, 4263348324u };
  struct S st10 = { 1121312628u, 2742391961u, 123861163u };
  cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)((unsigned)(s3)) | 0u))) / ((unsigned)(st9.f1) | 1u))));
  u4 = (unsigned)((unsigned)(s3)) & 0xffffffffu;
  for (unsigned g12 = 0u; g12 < 12u; g12++) {
    unsigned i11 = g12;
    cs = csmix(cs, i11);
    { struct SB4 sba13 = { (unsigned)(193101058u) & 0xffffffffu };
      struct SB4 sbt14 = sbh2(sba13, (unsigned)(((unsigned)((((unsigned)(((unsigned)(((unsigned)(i11) & (unsigned)(3711505683u))) >> ((unsigned)(((unsigned)(u5) << ((unsigned)(u4) & 31u))) & 31u))) & 1u) ? (unsigned)(st10.f1) : (unsigned)(helper1(((unsigned)(st9.f0) << ((unsigned)(u5) & 31u)), (unsigned)(s3))))) / ((unsigned)(((unsigned)(((unsigned)(((unsigned)(2211600856u) == ((unsigned)(u5) ^ cs))) ^ (unsigned)(((unsigned)(3247555879u) + (unsigned)(u8))))) - (unsigned)(u5))) | 1u))));
      cs = csmix(cs, sbt14.a);
    }
  }
  { unsigned g16 = 0u;
    while (g16 < 4u) {
      unsigned i15 = g16;
      cs = csmix(cs, i15);
      { unsigned g18 = 0u;
        while (g18 < 3u) {
          unsigned i17 = g18;
          cs = csmix(cs, i17);
          { struct SB4 sba19 = { (unsigned)(((unsigned)(3780064701u) - (unsigned)(4153290445u))) & 0xffffffffu };
            struct SB4 sbt20 = sbh2(sba19, (unsigned)(helper1((-((unsigned)(((unsigned)(u5) ^ (unsigned)(u6))) | 0u)), ((unsigned)(596510082u) | (unsigned)(((unsigned)(1647484201u) % ((unsigned)((unsigned)(s3)) | 1u)))))));
            cs = csmix(cs, sbt20.a);
          }
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((~((unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)(st10.f0) | 1u))) | 0u))) | (unsigned)((~((unsigned)(2479024286u) | 0u))))) - (unsigned)(3612638142u))));
          g18++;
        }
      }
      for (unsigned g22 = 0u; g22 < 11u; g22++) {
        unsigned i21 = g22;
        cs = csmix(cs, i21);
        { union UB ub23; ub23.w = (unsigned)((~((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) >> ((unsigned)(3059905207u) & 31u))) + (unsigned)(((unsigned)(2166352561u) * (unsigned)(i15))))) | 0u))); cs = csmix(cs, ub23.w); }
        cs = csmix(cs, (unsigned)((~((unsigned)((unsigned)(s3)) | 0u))));
        { union UB ub24; ub24.w = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u4) % ((unsigned)(82717919u) | 1u))) + (unsigned)(4032098929u))) + (unsigned)(i21))); cs = csmix(cs, ub24.w); }
      }
      st9.f2 = (unsigned)((~((unsigned)((~((unsigned)(u4) | 0u))) | 0u)));
      g16++;
    }
  }
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  { struct SB4 sba25 = { 1u };
    struct SB4 sbt26 = sbh2(sba25, cs);
    cs = csmix(cs, sbt26.a); }
  printf("checksum=%08x\n", cs);
  return 0;
}
