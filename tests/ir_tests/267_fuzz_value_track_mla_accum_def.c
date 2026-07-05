#include <stdio.h>

/*
 * Fuzz struct_byval seed 9494 (O2-only wrong checksum, batch-context).
 *
 * Root cause: value_tracking's generic "mark source operand reads" block
 * only consumed src1/src2, so an MLA whose src2 is a StackLoc read (matching
 * neither Pattern 2, which needs an immediate src2, nor Pattern 2a, which
 * excludes MLA) never marked its ACCUMULATOR VAR as read.  When the same VAR
 * was later redefined with another constant (u8 = u9), the pass NOP'd the
 * accumulator's defining instruction as an "unread constant def", leaving
 * `mla rd, rn, rm, ra` reading whatever the caller left in ra.
 *
 * The payload runs in a static helper, NOT in main, and main printf()s
 * first: at crt0 entry the dead register happened to hold a benign value,
 * so the bug only shows one call frame deep with dirtied caller registers
 * (exactly how the batched fuzz runner caught it — it prints a seed marker
 * before calling each seed's renamed main).
 * Fix: clear the accumulator's def in the generic read-marking block via
 * ir_opt_mla_accum_vreg (same blind spot as ptr seed 6869 / test 257).
 */
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
  lr = (unsigned)((~((unsigned)(pb) | 0u)));
  lr = (unsigned)(((unsigned)(((unsigned)(3599754057u) & (unsigned)(pa))) / ((unsigned)(3042614786u) | 1u)));
  if ((unsigned)(3747847367u) & 1u) lr += (unsigned)(lr);
  lr = (unsigned)(((unsigned)(pa) & (unsigned)(((unsigned)(((unsigned)(pb) >> ((unsigned)(lr) & 31u))) / ((unsigned)(3194511853u) | 1u)))));
  lr = (unsigned)(((unsigned)(lr) + (unsigned)(167010447u)));
  return (unsigned)(((unsigned)(((unsigned)(3310546613u) << ((unsigned)(lr) & 31u))) >> ((unsigned)(pb) & 31u))) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)((~((unsigned)(((unsigned)(997099101u) >= ((unsigned)(1042707764u) ^ lr))) | 0u)));
  if ((unsigned)(((unsigned)(helper1(1651480327u, 1960309723u)) >> ((unsigned)(((unsigned)(3814486492u) << ((unsigned)(44936666u) & 31u))) & 31u))) & 1u) lr += (unsigned)(pa);
  if ((unsigned)(3886238524u) & 1u) lr += (unsigned)(((unsigned)(2569495745u) * (unsigned)(((unsigned)(701868679u) >> ((unsigned)(pa) & 31u)))));
  lr = (unsigned)(pa);
  return (unsigned)(((unsigned)(pb) ^ (unsigned)(((unsigned)(((unsigned)(2635866373u) == ((unsigned)(3787006491u) ^ lr))) / ((unsigned)((((unsigned)(3331950001u) & 1u) ? (unsigned)(1075234276u) : (unsigned)(pb))) | 1u))))) ^ lr;
}

struct SB1 { unsigned char a; };

struct SB4 { unsigned a; };

struct SB5 { unsigned a; unsigned char b; };

struct SB8 { unsigned a; unsigned b; };

union UB { unsigned w; unsigned char b; };

static struct SB1 sbh3(struct SB5 p, unsigned x)
{
  struct SB1 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffu };
  r.a = (unsigned)(((unsigned)(3756227015u) | (unsigned)(((unsigned)(((unsigned)(1255879674u) < ((unsigned)(x) ^ x))) % ((unsigned)(p.a) | 1u))))) & 0xffu;
  r.a = (unsigned)(((unsigned)(((unsigned)(2307334002u) + (unsigned)(p.b))) - (unsigned)((~((unsigned)(((unsigned)(2537949626u) == ((unsigned)(x) ^ x))) | 0u))))) & 0xffu;
  r.a = (unsigned)(((unsigned)(((unsigned)(4274205254u) ^ (unsigned)((~((unsigned)(p.b) | 0u))))) + (unsigned)(((unsigned)((-((unsigned)(18260883u) | 0u))) + (unsigned)(p.a))))) & 0xffu;
  return r;
}

static struct SB4 sbh4(struct SB4 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
  r.a = (unsigned)(x) & 0xffffffffu;
  r.a = (unsigned)(1063560789u) & 0xffffffffu;
  return r;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

static void compute(void)
{
  unsigned cs = 0x12345678u;
  short s5 = (short)(1782523098u & 0xffff);
  int s6 = (int)(1435544645u & 0xffffffff);
  short s7 = (short)(961053802u & 0xffff);
  unsigned u8 = 1365077935u;
  unsigned u9 = 3067548818u;
  unsigned u10 = 2791409071u;
  unsigned u11 = 1965104597u;
  unsigned u12 = 2543407395u;
  unsigned arr13[8] = { 2046664754u, 1083734119u, 4137367771u, 2640292828u, 1442634789u, 3408787026u, 2917769676u, 3788118501u };

  { struct SB4 sba14 = { (unsigned)(1274545525u) & 0xffffffffu };
    struct SB4 sbt15 = sbh4(sba14, (unsigned)((-((unsigned)(((unsigned)(3341998762u) / ((unsigned)(2697686006u) | 1u))) | 0u))));
    cs = csmix(cs, sbt15.a);
  }
  { struct SB4 sba16 = { (unsigned)(u10) & 0xffffffffu };
    struct SB4 sbt17 = sbh4(sba16, (unsigned)((~((unsigned)(((unsigned)(u8) | (unsigned)(((unsigned)(((unsigned)(u8) * (unsigned)(arr13[((unsigned)(1836981046u) & 7u)]))) << ((unsigned)(3933568867u) & 31u))))) | 0u))));
    cs = csmix(cs, sbt17.a);
  }
  u10 = (unsigned)(helper1(((unsigned)(((unsigned)(((unsigned)(u11) % ((unsigned)(u10) | 1u))) * (unsigned)(arr13[((unsigned)(2328706780u) & 7u)]))) + (unsigned)(u8)), 3658283786u)) & 0xffffffffu;
  u12 = (unsigned)(((unsigned)(((unsigned)((~((unsigned)(u10) | 0u))) & (unsigned)(u8))) != ((unsigned)(u12) ^ cs))) & 0xffffffffu;
  cs = csmix(cs, (unsigned)(1371997856u));
  u8 = (unsigned)(u9) & 0xffffffffu;

  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, u10);
  cs = csmix(cs, u11);
  cs = csmix(cs, u12);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s5);
  cs = csmix(cs, (unsigned)s6);
  cs = csmix(cs, (unsigned)s7);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr13[k]);
  { struct SB5 sba18 = { 1u, 2u };
    struct SB1 sbt19 = sbh3(sba18, cs);
    cs = csmix(cs, sbt19.a); }
  { struct SB4 sba20 = { 19088744u };
    struct SB4 sbt21 = sbh4(sba20, cs);
    cs = csmix(cs, sbt21.a); }
  printf("checksum=%08x\n", cs);
}

int main(void)
{
  printf("start\n");
  compute();
  return 0;
}
