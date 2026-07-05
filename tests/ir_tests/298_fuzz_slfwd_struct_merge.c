/* struct_byval fuzz seed 60351 (O2): same store-load-forwarding
 * multiply-defined `?:` merge-temp root cause as ptr seed 72674 (test 297),
 * reached from the struct-by-value profile.  The ternary
 *   sbh2(sba15, (u6 & 1) ? (u6 ^ (u6-k)) / d : helper1(...))
 * builds a merge temp T (defined on both arms of the `?:` diamond) that is
 * stored into a by-value struct argument slot.  sl_forward
 * (ir/opt_memory.c tcc_ir_opt_sl_forward) forwarded T's else-arm def from a
 * LOAD, which re-marked fwd_tmp_valid[T]=1 without honouring the pre-scan's
 * multi-def rejection; the by-value store `V <- T` then resolved through the
 * single-arm value, so downstream loads/csmix read the wrong ternary arm.
 * Fixed by keeping fwd_tmp_defs live for the whole pass and re-checking
 * fwd_tmp_defs[t] < 2 at every fwd_tmp_val consume site. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)((-((unsigned)(pb) | 0u))) ^ lr;
}
struct SB1 { unsigned char a; };
struct SB4 { unsigned a; };
struct SB5 { unsigned a; unsigned char b; };
struct SB8 { unsigned a; unsigned b; };
union UB { unsigned w; unsigned char b; };
static struct SB4 sbh2(struct SB8 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
  r.a = (unsigned)(x) & 0xffffffffu;
  return r;
}
static struct SB8 sbh3(struct SB5 p, unsigned x)
{
  struct SB8 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu, (unsigned)(1717302362u) & 0xffffffffu };
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
  int s4 = (int)(1695503641u & 0xffffffff);
  unsigned u5 = 1822929492u;
  unsigned u6 = 3541335111u;
  struct S st7 = { 1837255706u, 2769512992u, 3871070919u };
  struct S st8 = { 634110740u, 438100413u, 1262545929u };
  { union UB ub9; ub9.w = (unsigned)(((unsigned)((~((unsigned)(((unsigned)(st7.f1) | (unsigned)(u6))) | 0u))) + (unsigned)(((unsigned)(((unsigned)(2348663032u) % ((unsigned)(u6) | 1u))) != ((unsigned)(((unsigned)(u5) - (unsigned)(u6))) ^ cs))))); cs = csmix(cs, ub9.w); }
  if ((unsigned)(2456496033u) & 1u) {
    { struct SB5 sba10 = { (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) - (unsigned)(st7.f0))) % ((unsigned)(3056884825u) | 1u))) & 0xffffffffu, (unsigned)(u6) & 0xffu };
      struct SB8 sbt11 = sbh3(sba10, (unsigned)(u5));
      cs = csmix(cs, sbt11.a);
      cs = csmix(cs, sbt11.b);
    }
    { union UB ub12; ub12.w = (unsigned)((unsigned)(s4)); cs = csmix(cs, ub12.w); }
    for (unsigned g14 = 0u; g14 < 5u; g14++) {
      unsigned i13 = g14;
      cs = csmix(cs, i13);
      cs = csmix(cs, (unsigned)(((unsigned)((((unsigned)((unsigned)(s4)) & 1u) ? (unsigned)((-((unsigned)((((unsigned)((unsigned)(s4)) & 1u) ? (unsigned)(i13) : (unsigned)(((unsigned)(i13) ^ cs)))) | 0u))) : (unsigned)(4250390656u))) & (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) - (unsigned)(1208618921u))) & (unsigned)(u5))) << ((unsigned)(((unsigned)(i13) - (unsigned)(((unsigned)(u6) ^ (unsigned)(286218182u))))) & 31u))))));
      cs = csmix(cs, (unsigned)(4246990261u));
      { struct SB8 sba15 = { (unsigned)(3168090412u) & 0xffffffffu, (unsigned)(((unsigned)((unsigned)(s4)) + (unsigned)(i13))) & 0xffffffffu };
        struct SB4 sbt16 = sbh2(sba15, (unsigned)((((unsigned)(u6) & 1u) ? (unsigned)(((unsigned)(((unsigned)(u6) ^ (unsigned)(((unsigned)(u6) - (unsigned)(321617559u))))) / ((unsigned)(((unsigned)(((unsigned)(u5) % ((unsigned)(3935532043u) | 1u))) - (unsigned)(st8.f0))) | 1u))) : (unsigned)(helper1(4177343971u, (~((unsigned)((~((unsigned)(u6) | 0u))) | 0u)))))));
        cs = csmix(cs, sbt16.a);
      }
      cs = csmix(cs, (unsigned)((unsigned)(s4)));
      { struct SB8 sba17 = { (unsigned)(st8.f2) & 0xffffffffu, (unsigned)((unsigned)(s4)) & 0xffffffffu };
        struct SB4 sbt18 = sbh2(sba17, (unsigned)(((unsigned)(178493748u) - (unsigned)(helper1(((unsigned)(u6) > ((unsigned)((~((unsigned)(3838543863u) | 0u))) ^ cs)), (-((unsigned)(((unsigned)((unsigned)(s4)) / ((unsigned)(i13) | 1u))) | 0u)))))));
        cs = csmix(cs, sbt18.a);
      }
    }
  }
  u6 = (unsigned)(((unsigned)(u6) + (unsigned)(2878181396u))) & 0xffffffffu;
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  cs = csmix(cs, st8.f0);
  cs = csmix(cs, st8.f1);
  cs = csmix(cs, st8.f2);
  { struct SB8 sba19 = { 1u, 2u };
    struct SB4 sbt20 = sbh2(sba19, cs);
    cs = csmix(cs, sbt20.a); }
  { struct SB5 sba21 = { 19088744u, 105u };
    struct SB8 sbt22 = sbh3(sba21, cs);
    cs = csmix(cs, sbt22.a);
    cs = csmix(cs, sbt22.b); }
  printf("checksum=%08x\n", cs);
  return 0;
}
