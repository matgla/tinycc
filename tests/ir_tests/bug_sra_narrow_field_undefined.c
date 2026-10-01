/* A struct field scalar replacement pulls out of the frame has to be DEFINED
 * where the struct was written.
 *
 * SRA gives each field of a promotable frame object its own VAR.  SSA only
 * counts a full-width slot STORE as a definition of a variable; a narrower one
 * updates whatever name is current, in place.  A one-byte field written only
 * by byte stores therefore got a VAR that was never defined at all, while its
 * reads -- which SSA rename turns into whole-register copies -- took the
 * undefined entry name.  Here `sba25` is built and passed by value, and at -O2
 * the callee read three bytes of whatever was under it; the loop and the
 * dirtied stack are what make that show rather than pass by luck.
 *
 * Reduced from struct_byval fuzz seed 1962. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(lr) ^ lr;
}
struct SB1 { unsigned char a; };
struct SB4 { unsigned a; };
union UB { unsigned w; unsigned char b; };
static struct SB4 sbh2(struct SB1 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
  r.a = (unsigned)(p.a) & 0xffffffffu;
  return r;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
__attribute__((noinline)) static void dirty(int d)
{
  volatile unsigned b[64];
  for (int i = 0; i < 64; i++)
    b[i] = 0xDEAD0000u + (unsigned)(d * 64 + i);
  if (d)
    dirty(d - 1);
}

__attribute__((noinline)) static unsigned body(void)
{
  unsigned cs = 0x12345678u;
  unsigned char s5 = (unsigned char)(1863726238u & 0xff);
  unsigned u6 = 844757540u;
  unsigned u7 = 2753348224u;
  unsigned u8 = 1393046211u;
  unsigned u9 = 1308701682u;
  unsigned u10 = 2566731158u;
  unsigned u11 = 3567405196u;
  unsigned arr12[8] = { 1016393539u, 2923799514u, 1914012732u, 4085784861u, 2376755388u, 1239331789u, 1682676424u, 174298200u };
  unsigned arr13[8] = { 3495248964u, 3150037832u, 2562321627u, 3316459955u, 3242667970u, 1555379116u, 3857701763u, 1520688340u };
  struct S st14 = { 3412160013u, 2070586499u, 1763051845u };
  struct S st15 = { 2628473315u, 715880564u, 3847184839u };
  for (unsigned g18 = 0u; g18 < 5u; g18++) {
    unsigned i17 = g18;
    cs = csmix(cs, i17);
    { union UB ub19; ub19.w = (unsigned)(u9); cs = csmix(cs, ub19.w); }
    u9 = (unsigned)(u7) & 0xffffffffu;
    for (unsigned g21 = 0u; g21 < 8u; g21++) {
      unsigned i20 = g21;
      cs = csmix(cs, i20);
      { union UB ub22; ub22.w = (unsigned)(((unsigned)((unsigned)(s5)) & (unsigned)(((unsigned)(((unsigned)(u8) >> ((unsigned)(589226152u) & 31u))) | (unsigned)(((unsigned)(i17) << ((unsigned)(st15.f1) & 31u))))))); cs = csmix(cs, ub22.w); }
      cs = csmix(cs, (unsigned)(((unsigned)(arr13[((unsigned)(u11) & 7u)]) - (unsigned)(((unsigned)(((unsigned)(((unsigned)(i20) - (unsigned)(u10))) ^ (unsigned)(st14.f0))) << ((unsigned)(((unsigned)(((unsigned)(u11) & (unsigned)(4196820644u))) << ((unsigned)((unsigned)(s5)) & 31u))) & 31u))))));
    }
    arr13[((unsigned)(u8) & 7u)] = (unsigned)(u7);
  }
  { struct SB1 sba25 = { (unsigned)(u9) & 0xffu };
    struct SB4 sbt26 = sbh2(sba25, (unsigned)(((unsigned)(((unsigned)((((unsigned)(u11) & 1u) ? (unsigned)(helper1(480138227u, st14.f0)) : (unsigned)(((unsigned)((unsigned)(s5)) / ((unsigned)(u7) | 1u))))) % ((unsigned)((-((unsigned)(u6) | 0u))) | 1u))) % ((unsigned)((~((unsigned)(((unsigned)(((unsigned)(arr12[((unsigned)(4142406418u) & 7u)]) << ((unsigned)(3576979367u) & 31u))) * (unsigned)((unsigned)(s5)))) | 0u))) | 1u))));
    cs = csmix(cs, sbt26.a);
  }
  cs = csmix(cs, u9);
  return cs;
}

int main(void)
{
  dirty(6);
  printf("checksum=%08x\n", body());
  return 0;
}
