/* Guard: a full-width slot STORE to an upward-exposed ("global") local is a
 * fresh SSA definition, so phi placement must see it.
 *
 * The renamer rewrites `v = <value>` in STORE form to an ASSIGN with a FRESH
 * SSA name (ssa_store_slot_def_pos in ir/ssa.c).  Before, that only happened
 * for block-local vars: phi placement skipped STORE defs entirely, so a fresh
 * name created on one arm of a branch would be lost at the join.  Now the def
 * scan and the renamer share one predicate and phis are placed for STORE defs
 * too, which is what lets const/copy propagation and SCCP thread values
 * through vars the frontend writes in STORE form (304_fuzz went 391 -> 6
 * instructions, GCC parity, on the back of this).
 *
 * Each case below writes the var on one or both arms of a branch / around a
 * loop and reads it after the merge, so a dropped or misplaced phi shows up as
 * a wrong value rather than only as a code-size change.  Values are chosen so
 * that a stale pre-branch value, a value from the wrong arm, and a value from
 * the wrong iteration are all distinguishable.
 */
#include <stdio.h>

/* Opaque to the optimizer: keeps the runtime paths from folding to constants
 * so both the compile-time-folded and the runtime shapes are exercised. */
static int opaque(int v) { return v; }

/* Both arms redefine `u`; the read after the join must see the taken arm. */
static unsigned join_both_arms(int sel)
{
  unsigned u = 11u;
  if (sel)
    u = 22u;
  else
    u = 33u;
  return u * 10u + (unsigned)sel;
}

/* Only one arm redefines `u`: the join phi must merge the new name with the
 * pre-branch one. */
static unsigned join_one_arm(int sel)
{
  unsigned u = 44u;
  if (sel)
    u = 55u;
  return u;
}

/* Loop-carried: the header phi must merge the entry def with the back-edge
 * STORE def, or the loop reads iteration 0's value forever. */
static unsigned loop_carried(int n)
{
  unsigned acc = 1u;
  for (int i = 0; i < n; i++)
    acc = acc * 3u + 1u;
  return acc;
}

/* Address-taken alias: `p` writes the same slot, so the var must NOT be
 * promoted and the read after the store must observe the pointer write.
 * (Mirrors the 304_fuzz shape once ptr_local_fwd has forwarded `p`.) */
static unsigned aliased_store(int sel)
{
  unsigned u = 7u;
  unsigned *p = &u;
  if (sel)
    u = 8u;
  *p = u + 100u;
  return u;
}

/* Two independent vars written in STORE form on opposite arms: their phi webs
 * must not be conflated. */
static unsigned two_vars(int sel)
{
  unsigned a = 1u, b = 2u;
  if (sel)
    a = 100u;
  else
    b = 200u;
  return a * 1000u + b;
}

int main(void)
{
  printf("join_both_arms: %u %u\n", join_both_arms(1), join_both_arms(0));
  printf("join_one_arm: %u %u\n", join_one_arm(1), join_one_arm(0));
  printf("loop_carried: %u %u %u\n", loop_carried(0), loop_carried(1),
         loop_carried(5));
  printf("aliased_store: %u %u\n", aliased_store(1), aliased_store(0));
  printf("two_vars: %u %u\n", two_vars(1), two_vars(0));

  /* Same shapes again, this time with runtime-unknown selectors so the folded
   * and the register-allocated paths are both covered. */
  int s1 = opaque(1), s0 = opaque(0), n5 = opaque(5);
  printf("rt join_both_arms: %u %u\n", join_both_arms(s1), join_both_arms(s0));
  printf("rt join_one_arm: %u %u\n", join_one_arm(s1), join_one_arm(s0));
  printf("rt loop_carried: %u\n", loop_carried(n5));
  printf("rt aliased_store: %u %u\n", aliased_store(s1), aliased_store(s0));
  printf("rt two_vars: %u %u\n", two_vars(s1), two_vars(s0));
  return 0;
}
