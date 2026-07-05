/* switch fuzz seed 457962 (O2 wrong) — linear-scan register allocator.
 *
 * Root cause: two live intervals can legitimately share one hard register
 * when a phi/merge copy coalesces them (an if/else merge temp for a variable
 * inherits the register from one arm via its single hint_vreg, while the other
 * arm's interval is still live and also holds that register).  Here `u6`'s
 * merge temp (interval [104,165], live across the trailing g30 loop) shared R6
 * with the if-arm's loop-carried `u6` interval ([36,121]).  When the shorter
 * interval expired at the loop entry, ra_linear_scan's expire loop returned R6
 * to int_free unconditionally, even though the merge temp still occupied R6.
 * A loop-body temp (`u5 = st9.f2 | 146`) then grabbed the "free" R6, so after
 * the loop the final `csmix(cs, u6)` read u5's value instead of u6 — the
 * symptom was u6 == u5.
 *
 * Fix (ir/regalloc.c ra_linear_scan): before returning an expiring interval's
 * register to the free pool, skip it if any surviving (still-live) active
 * interval still holds that register — never free a register a coalesced
 * partner still occupies.
 *
 * Correct checksum (tcc -O0 == -O1 == -O2 == -Os): e4d996fe.
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
  return (unsigned)((~((unsigned)(((unsigned)((-((unsigned)(181849284u) | 0u))) - (unsigned)(((unsigned)(2583303748u) + (unsigned)(841233755u))))) | 0u))) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(((unsigned)(helper1(3834065517u, pa)) - (unsigned)(((unsigned)(pa) | (unsigned)(2271169006u))))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(lr) - (unsigned)(pa))) + (unsigned)(((unsigned)(1394889528u) << ((unsigned)(4105357576u) & 31u)))));
  return (unsigned)(((unsigned)(3940828236u) * (unsigned)(((unsigned)(helper1(3058974259u, 4280340189u)) >> ((unsigned)(((unsigned)(pa) + (unsigned)(((unsigned)(pa) ^ lr)))) & 31u))))) ^ lr;
}

static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(pb) << ((unsigned)(((unsigned)((~((unsigned)(1284343770u) | 0u))) / ((unsigned)(helper1(4033060081u, pa)) | 1u))) & 31u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  char s4 = (char)(1735976082u & 0xff);
  unsigned u5 = 3942434314u;
  unsigned u6 = 2953729614u;
  unsigned u7 = 3600241619u;
  struct S st8 = { 4278469532u, 651828113u, 588160997u };
  struct S st9 = { 431170223u, 1061360541u, 3787670158u };

  if ((unsigned)((((unsigned)(((unsigned)((-((unsigned)(st8.f1) | 0u))) / ((unsigned)(u5) | 1u))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(((unsigned)(u6) ^ (unsigned)(((unsigned)(u6) ^ cs)))) ^ (unsigned)(1475527521u))) >> ((unsigned)(((unsigned)(2775725972u) - (unsigned)(st8.f1))) & 31u))) : (unsigned)((~((unsigned)((~((unsigned)(((unsigned)(st9.f0) >> ((unsigned)(st8.f1) & 31u))) | 0u))) | 0u))))) & 1u) {
    for (unsigned g11 = 0u; g11 < 5u; g11++) {
      st8.f0 = (unsigned)(((unsigned)((~((unsigned)(helper2(((unsigned)(st8.f0) >> ((unsigned)(((unsigned)(st8.f0) ^ cs)) & 31u)), ((unsigned)(1531987012u) - (unsigned)(u7)))) | 0u))) << ((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) + (unsigned)(1498184110u))) & (unsigned)(((unsigned)(st8.f1) | (unsigned)(st8.f0))))) - (unsigned)(u5))) & 31u)));
      u6 = (unsigned)((-((unsigned)(u5) | 0u))) & 0xffffffffu;
    }
  } else {
    cs = csmix(cs, (unsigned)(((unsigned)((((unsigned)(u5) & 1u) ? (unsigned)((unsigned)(s4)) : (unsigned)(((unsigned)(((unsigned)(u6) * (unsigned)(((unsigned)(u6) ^ cs)))) & (unsigned)(st8.f1))))) * (unsigned)((((unsigned)(st9.f0) & 1u) ? (unsigned)(((unsigned)(((unsigned)(st8.f0) + (unsigned)(1501363847u))) & (unsigned)((-((unsigned)(st8.f2) | 0u))))) : (unsigned)(((unsigned)(1195054176u) % ((unsigned)(((unsigned)(u7) << ((unsigned)(u5) & 31u))) | 1u))))))));
  }
  { unsigned sel20 = (unsigned)(((unsigned)(((unsigned)(2405577853u) - (unsigned)(2194876764u))) & (unsigned)(919027277u))) & 3u;
    switch (sel20) {
    default: cs = csmix(cs, 246u); break;
    } }
  for (unsigned g30 = 0u; g30 < 5u; g30++) {
    unsigned i29 = g30;
    cs = csmix(cs, i29);
    { unsigned g32 = 0u;
      while (g32 < 11u) {
        u5 = (unsigned)(((unsigned)(st9.f2) | (unsigned)((((unsigned)(((unsigned)(2039114990u) ^ (unsigned)(174939263u))) & 1u) ? (unsigned)((unsigned)(s4)) : (unsigned)(((unsigned)(401117349u) / ((unsigned)((((unsigned)(st8.f0) & 1u) ? (unsigned)(u6) : (unsigned)(i29))) | 1u))))))) & 0xffffffffu;
        g32++;
      }
    }
  }

  cs = csmix(cs, u6);
  printf("checksum=%08x\n", cs);
  return 0;
}
