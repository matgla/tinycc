/* Fuzz seed struct_byval:76 (-O2, also combo:2718): dom-LICM marks a whole
 * invariant chain (T20 <- hoisted_call SUB c; T21 <- c SUB T20) but the
 * EMISSION loop can skip a member (speculative single-use gate `hu < 2`, or
 * the hoist budget) while still hoisting a later member that USES its dest —
 * the preheader then computes `T21 = c SUB T20` before T20's def (left in the
 * loop) ever ran, reading garbage.  Fix: track actually-hoisted dests during
 * emission and never hoist an instruction whose in-loop-defined source has
 * not itself been hoisted. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(((unsigned)(pb) + (unsigned)(((unsigned)(pb) ^ lr)))) & 1u) lr += (unsigned)(((unsigned)(pa) ^ (unsigned)((((unsigned)(657219029u) & 1u) ? (unsigned)(pa) : (unsigned)(96383740u)))));
  return (unsigned)((-((unsigned)(((unsigned)(((unsigned)(2111625427u) - (unsigned)(3391131917u))) & (unsigned)(((unsigned)(3471955333u) * (unsigned)(lr))))) | 0u))) ^ lr;
}
struct SB4 { unsigned a; };
struct SB5 { unsigned a; unsigned char b; };
union UB { unsigned w; unsigned char b; };
static struct SB4 sbh5(struct SB5 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
  r.a = (unsigned)((-((unsigned)(((unsigned)(3721469660u) % ((unsigned)(((unsigned)(x) >> ((unsigned)(p.a) & 31u))) | 1u))) | 0u))) & 0xffffffffu;
  return r;
}
int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u10 = 2456088050u;
  unsigned u11 = 2481167710u;
      { unsigned g18 = 0u;
    while (g18 < 3u) {
      unsigned i17 = g18;
      cs = csmix(cs, i17);
      { unsigned g20 = 0u;
        while (g20 < 4u) {
          unsigned i19 = g20;
          cs = csmix(cs, i19);
          { union UB ub21; ub21.w = (unsigned)(((unsigned)(u10) - (unsigned)(((unsigned)(helper1(3150356555u, u11)) - (unsigned)(4114999132u))))); cs = csmix(cs, ub21.w); }
          g20++;
        }
      }
                  g18++;
    }
  }
  cs = csmix(cs, u10);
  cs = csmix(cs, u11);
      { struct SB5 sba29 = { 38177487u, 208u };
    struct SB4 sbt30 = sbh5(sba29, cs);
    cs = csmix(cs, sbt30.a); }
  printf("checksum=%08x\n", cs);
  return 0;
}
