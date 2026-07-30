/* Fuzz seed int:3453 (-O2): guard_collapse's jump-retarget cleanup (forward
 * JMP-over-NOPs) ran inside REGALLOC's SSA pipeline whenever its own folds
 * reported changes — but there ssa->block_phis still references the
 * construction-time CFG.  The retargeted JUMPIF then pointed into the middle
 * of a stale block, ra_resolve_phis could not match the phi edge
 * (target_block/fallthrough_block filters both missed), and the else-arm phi
 * copy was silently dropped; ra_eliminate_dead_reg_copies later NOP'd the
 * now-unread else-arm constant def, so the branch-taken path read an
 * undefined register for u6.  Fix: the cleanup only runs in the driver
 * context (idle_cleanup callers), where the IR is phi-free flat form. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = ((pb / (pa | 1u)) | 3177855982u) + ~3630368017u;
  return (unsigned)(((unsigned)(2264382279u) + (unsigned)(((unsigned)(lr) >> ((unsigned)((-((unsigned)(pb) | 0u))) & 31u))))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  long s2 = (long)(1469823815u & 0xffffffff);
  long s3 = (long)(782998528u & 0xffffffff);
  char s4 = (char)(1744906777u & 0xff);
  unsigned u5 = 3411454300u;
  unsigned u6 = 2525890377u;
  struct S st7 = { 1534655125u, 2668044467u, 2779791322u };
  if (((unsigned)s4 - (-u5 * (u6 << ((u6 ^ cs) & 31u))) * (unsigned)s4) & 1u) {
    u6 = (((2257700544u * u6) % (st7.f2 | 1u)) * 3647143371u);
    if (((helper1(u5 + (u5 ^ cs), ~(unsigned)s2) >> 29) + ((unsigned)s2 << (~helper1((unsigned)s2, u6) & 31u))) & 1u) {
      u6 = 1189884263u;
    }
    cs = csmix(cs, (unsigned)(3135177379u));
  } else {
    { unsigned g9 = 0u;
      while (g9 < 7u) {
        unsigned i8 = g9;
        cs = csmix(cs, i8);
        g9++;
      }
    }
  }
  cs = csmix(cs, -(unsigned)(((unsigned)s2 ^ (u6 < ((u6 ^ cs) ^ cs))) == ((~2748655208u ^ u5) ^ cs)));
  st7.f2 = (unsigned)((unsigned)(s4));
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
