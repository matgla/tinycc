/* Regression: loop_const_sim's rotated-loop range extension absorbed the ELSE
 * arm of a guard whose THEN arm held the collapsible loop, deleting the else
 * body and misrouting the guard's false-branch jump straight to the exit.
 *
 * From longlong fuzz seed 2426 (reduced).  tcc -O0/-O1 agreed with gcc; only
 * tcc -O2 diverged — an O2 optimizer miscompile.  Culprit knobs (bisect):
 * const-prop, jump-threading, loop-unroll (loop_const_sim is gated by
 * opt_loop_unroll).
 *
 * Root cause: `if (guard) { while(...) {..} } else { cs = csmix(...); }`.  The
 * while loop lives in the THEN arm; the guard's false-branch JUMP targets the
 * ELSE block, which is laid out AFTER the loop's back-edge but BEFORE the join
 * (the loop exit target).  lcs_try_fold (ir/opt_loop_const_sim.c) extends the
 * effective loop range to [start .. exit_target-1] to catch rotated loop
 * bodies, but that span here swallowed the else block.  The pass then NOP'd the
 * whole span and re-emitted only the loop's residual, deleting the else's
 * `cs = csmix(cs, q7<q10?1:0)` and retargeting the guard-false jump to the
 * tail — so at runtime (guard is false) the else csmix was skipped, producing
 * a checksum for one fewer csmix call.
 *
 * The guard folds to a compile-time constant only after inlining
 * helper2/helper1 (both take constant args), which is why const-prop is a
 * culprit; the 64-bit `q7 < q10` in the else and the constant-trip while loop
 * exercise the exact shape.
 *
 * Fix: after extending eff_end to exit_target-1, re-check that no instruction
 * OUTSIDE the (now-extended) loop range jumps INTO the newly-absorbed tail; if
 * one does (the guard's false-branch entry into the else block), bail out of
 * the fold.  The caller's ext_entry check only covered the pre-extension range.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  return h * 2654435761u;
}

static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(lr) != ((unsigned)(((unsigned)((((unsigned)(pa) & 1u) ? (unsigned)(pb) : (unsigned)(lr))) | (unsigned)(pb))) ^ lr))) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if ((unsigned)(((unsigned)((((unsigned)(pb) & 1u) ? (unsigned)(pa) : (unsigned)(2879696197u))) | (unsigned)(1644492581u))) & 1u) lr += (unsigned)(((unsigned)(helper1(pa, pb)) * (unsigned)((~((unsigned)(pb) | 0u)))));
  return (unsigned)(((unsigned)((-((unsigned)(pb) | 0u))) >> ((unsigned)(((unsigned)(((unsigned)(2070594274u) % ((unsigned)(pa) | 1u))) - (unsigned)((~((unsigned)(2788821432u) | 0u))))) & 31u))) ^ lr;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u5 = 2771507666u;
  unsigned u6 = 3772369011u;
  unsigned long long q7 = (((unsigned long long)(u5)) << 32) | (unsigned long long)(u6);
  unsigned long long q10 = (((unsigned long long)(u6)) << 32) | (unsigned long long)(u5);
  if ((unsigned)((-((unsigned)(helper2(3386856634u, 2496130422u)) | 0u))) & 1u) {
    { unsigned g13 = 0u;
      while (g13 < 7u) {
        cs = csmix(cs, g13);
        g13++;
      }
    }
  } else {
    cs = csmix(cs, ((q7) < (q10)) ? 1u : 0u);  /* must NOT be dropped */
  }
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  printf("checksum=%08x\n", cs);
  return 0;
}
