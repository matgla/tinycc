/* fuzz switch seed 198468 (O1 divergence).
 * Pass: try_eliminate_loop (ir/opt_loop_utils.c).
 * Root cause: dead-loop elimination NOP'd the entire loop body (including the
 * exit branch) and relied on fall-through to reach the loop's exit target.
 * When the loop is the then-arm of an `if` its exit branch jumps *forward past
 * the else-arm*; NOPing it dropped control straight into the else block, so the
 * else `cs = csmix(cs, 212)` ran even though the (true) condition took the then
 * arm.  The empty `while (g14 < 7)` loop is trip-count-eliminated at O1.
 * Fix: when fall-through after the removed loop does not physically reach the
 * exit target, restore the exit edge with an explicit JUMP (need_exit_jump,
 * mirroring the loop unroller).
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  int s2 = (int)(1510035178u & 0xffffffff);
  unsigned u4 = 370227418u;
  unsigned u9 = 3466097076u;

  if ((unsigned)((~((unsigned)(((unsigned)((~((unsigned)(((unsigned)(1495318073u) ^ (unsigned)(u4))) | 0u))) >> ((unsigned)(((unsigned)(1062059329u) >= ((unsigned)((unsigned)(s2)) ^ cs))) & 31u))) | 0u))) & 1u) {
    { unsigned g14 = 0u;
      while (g14 < 7u) {
        g14++;
      }
    }
  } else {
    { unsigned g17 = (unsigned)(((unsigned)(((unsigned)(u9) >> ((unsigned)(((unsigned)(((unsigned)(u4) - (unsigned)(1500207665u))) - (unsigned)(3867143336u))) & 31u))) | (unsigned)(3184943293u))) & 1u;
      (void)g17;
      cs = csmix(cs, 212u); }
  }

  printf("checksum=%08x\n", cs);
  return 0;
}
