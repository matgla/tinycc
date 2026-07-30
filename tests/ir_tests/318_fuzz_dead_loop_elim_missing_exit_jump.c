/* fuzz switch seed 198468 (O1 divergence, second cause).
 * Historical pass: tcc_ir_opt_dead_loop_elim (ir/opt_dce.c) — retired 2026-07-07;
 * dead-loop collapse is now owned by ssa:dead_loop (ir/opt/ssa_opt_dead_loop.c),
 * whose try_kill_loop_body always writes an explicit `JUMP exit_target`, so this
 * bug class is structurally impossible on the SSA path.  Kept as an
 * anti-regression pin: the collapsed loop must not drop its exit edge.
 * Root cause: same class as 317 but a different eliminator.  A loop whose body
 * only assigns a constant to a VAR plus a counter (`u6 = s2 ^ const`, folded to
 * `u6 = #const` after const-prop) was removed by dead_loop_elim, which NOP'd the
 * whole body — including the forward exit branch — and hoisted the constant
 * into the preheader, relying on fall-through to the exit target.  As the loop
 * is the then-arm of an `if`, fall-through dropped into the else block, so the
 * else `csmix(cs, 212)` executed too.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  int s2 = (int)(1510035178u & 0xffffffff);
  unsigned u4 = 370227418u;
  unsigned u6 = 2555246728u;

  if ((unsigned)((~((unsigned)(((unsigned)((~((unsigned)(((unsigned)(1495318073u) ^ (unsigned)(u4))) | 0u))) >> ((unsigned)(((unsigned)(1062059329u) >= ((unsigned)((unsigned)(s2)) ^ cs))) & 31u))) | 0u))) & 1u) {
    unsigned g14 = 0u;
    while (g14 < 7u) {
      u6 = ((unsigned)s2 ^ 3543795714u);
      g14++;
    }
  } else {
    cs = csmix(cs, 212u);
  }
  { unsigned sel18 = (unsigned)s2 & 7u;
    switch (sel18) { default: cs = csmix(cs, 129u); break; } }
  cs = csmix(cs, u6);

  printf("checksum=%08x\n", cs);
  return 0;
}
