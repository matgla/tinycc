/* Regression for fuzz struct_byval seed 147471: LEA folding and redundant
 * store elimination must preserve struct-by-value temporary stores. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(pa) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(726074042u) << ((unsigned)(pa) & 31u))) ^ (unsigned)(2072120379u)));
  return (unsigned)(((unsigned)(3263113860u) & (unsigned)(((unsigned)(((unsigned)(pb) / ((unsigned)(2443765753u) | 1u))) >= ((unsigned)(882631710u) ^ lr))))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(3701631540u) / ((unsigned)(((unsigned)(pb) + (unsigned)(126097965u))) | 1u))) | (unsigned)(((unsigned)(((unsigned)(pa) / ((unsigned)(3180248503u) | 1u))) >> ((unsigned)(((unsigned)(lr) << ((unsigned)(2617295514u) & 31u))) & 31u))))) ^ lr;
}
static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(2404313333u) ^ lr;
}
struct SB1 { unsigned char a; };
struct SB4 { unsigned a; };
struct SB5 { unsigned a; unsigned char b; };
struct SB8 { unsigned a; unsigned b; };
static struct SB5 sbh4(struct SB5 p, unsigned x)
{
  struct SB5 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu, (unsigned)(p.b) & 0xffu };
  r.a = (unsigned)((~((unsigned)(((unsigned)(1124066536u) == ((unsigned)(((unsigned)(p.a) & (unsigned)(1517265745u))) ^ x))) | 0u))) & 0xffffffffu;
  return r;
}
static struct SB4 sbh5(struct SB5 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
  r.a = (unsigned)(((unsigned)((~((unsigned)((~((unsigned)(x) | 0u))) | 0u))) & (unsigned)(x))) & 0xffffffffu;
  return r;
}
static struct SB4 sbh6(struct SB4 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
  return r;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  short s7 = (short)(20996024u & 0xffff);
  short s8 = (short)(1807282953u & 0xffff);
  short s9 = (short)(1119670414u & 0xffff);
  unsigned u10 = 3561872772u;
  unsigned u11 = 3257351332u;
  unsigned u12 = 2073885575u;
  unsigned arr13[8] = { 2323627211u, 3911624890u, 2519064109u, 3853785163u, 827510289u, 620548126u, 1242591506u, 2224600955u };
  unsigned arr14[8] = { 4168106301u, 1561070032u, 950632876u, 422502u, 512648993u, 2075522190u, 3314767527u, 2183653828u };
  { struct SB5 sba15 = { (unsigned)(3626273379u) & 0xffffffffu, (unsigned)(((unsigned)(((unsigned)((unsigned)(s9)) << ((unsigned)(arr13[((unsigned)(u11) & 7u)]) & 31u))) - (unsigned)(2972877859u))) & 0xffu };
    struct SB4 sbt16 = sbh5(sba15, (unsigned)(((unsigned)(4266554666u) / ((unsigned)(((unsigned)(883113321u) / ((unsigned)(helper1(((unsigned)((unsigned)(s7)) & (unsigned)(120810254u)), ((unsigned)(2862432461u) << ((unsigned)(arr14[((unsigned)(98553268u) & 7u)]) & 31u)))) | 1u))) | 1u))));
    cs = csmix(cs, sbt16.a);
  }
  cs = csmix(cs, u10);
  cs = csmix(cs, u11);
  cs = csmix(cs, u12);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s7);
  cs = csmix(cs, (unsigned)s8);
  cs = csmix(cs, (unsigned)s9);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr13[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr14[k]);
  { struct SB5 sba17 = { 1u, 2u };
    struct SB5 sbt18 = sbh4(sba17, cs);
    cs = csmix(cs, sbt18.a);
    cs = csmix(cs, sbt18.b); }
  { struct SB5 sba19 = { 19088744u, 105u };
    struct SB4 sbt20 = sbh5(sba19, cs);
    cs = csmix(cs, sbt20.a); }
  { struct SB4 sba21 = { 38177487u };
    struct SB4 sbt22 = sbh6(sba21, cs);
    cs = csmix(cs, sbt22.a); }
  printf("checksum=%08x\n", cs);
  return 0;
}
