#include <stdio.h>

/*
 * Pure-call hoisting regression test (docs/bugs.md #7, sixth defect;
 * ptr fuzz seeds 500/517).
 *
 * `mix` is a pure function of its arguments, so tcc_ir_hoist_pure_calls
 * considers hoisting `mix(7u, u)` out of the loop.  The argument `u` is
 * never assigned directly inside the loop — but its ADDRESS is taken
 * (`p = &u`) and the loop mutates it through the pointer every iteration.
 * The original invariance check only scanned for direct defs of the vreg,
 * declared `u` loop-invariant, and hoisted the call: cs then accumulated
 * eight copies of mix(7, 100) instead of mix over 100,113,126,...
 *
 * The fix: an address-taken variable is only loop-invariant if the loop
 * provably cannot write memory (no stores, no non-CONST calls, no asm).
 */

/* Pure, but large enough that the inliner leaves the call in place. */
static unsigned
mix (unsigned a, unsigned b)
{
  unsigned t = b * 2654435761u;
  unsigned r = (a ^ t) + (b >> 3);
  r = r ^ (r >> 7);
  r = r * 97u + 13u;
  r = r ^ (b << 5);
  return r;
}

int
main (void)
{
  unsigned u = 100u;
  unsigned *p = &u;
  unsigned cs = 0u;
  int i;

  for (i = 0; i < 8; i++)
    {
      cs += mix (7u, u);  /* u is address-taken; mutated through *p below */
      *p = u + 13u;
    }

  printf ("cs=%u u=%u\n", cs, u);
  return 0;
}
