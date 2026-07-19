/* Guard: pruned SSA phi placement (ir/ssa.c, ssa_compute_live_in).
 *
 * Phis are now placed only where the VAR is live-in, not across the whole
 * iterated dominance frontier of its defs.  The win is `divmod_scan` below —
 * the gcc-torture arith-rand-ll shape, where each `{ unsigned xx = x, yy = y;
 * ... }` scope inside a loop is upward-exposed in its own second block (so
 * Briggs calls it global) yet is dead on every back-edge.  Those dead
 * loop-header + latch phi pairs used to be materialized as a slot-to-slot copy
 * on every edge into the latch: 845 of arith-rand-ll main's 1273 instructions.
 *
 * Everything else here pins the liveness itself.  Dropping a phi that IS
 * needed silently reads a value from the wrong predecessor, so each trap
 * checks a shape whose live-in set the analysis must NOT get wrong:
 *   - carried_over_continue: an accumulator live across a `continue` that
 *     skips its update — live-in at the header through the back-edge;
 *   - arm_define_join: defined on one arm only, read after the merge;
 *   - loop_carried_i64: a 64-bit var, which never takes a fresh SSA name
 *     (only a covering INT32 slot store does), so its writes update the value
 *     in place and must count as READS, not kills;
 *   - ptr_switch: a pointer var reassigned on one arm and dereferenced on the
 *     other — the store dest reads the var's value rather than its slot;
 *   - live_out_of_loop: written only under a condition inside the loop and
 *     read after it, so it is live-in at the header via the loop exit;
 *   - irreducible: a `goto` into the loop body, so the backward fixpoint runs
 *     over a multi-entry region with no single header.
 */
#include <stdio.h>

static int fails;

static void expect(const char *what, long long got, long long want)
{
  if (got != want)
  {
    printf("FAIL %s: got %lld want %lld\n", what, got, want);
    fails++;
  }
}

static unsigned long long seed = 47114711;

static unsigned long long nextrand(void)
{
  seed = seed * 1103515245ull + 12345ull;
  return seed >> 8;
}

/* The arith-rand-ll shape: nested block scopes, each with its own upward-
 * exposed {xx,yy,r1,r2}, each able to `continue` out of the iteration. */
static unsigned long long divmod_scan(int n)
{
  unsigned long long acc = 0;
  for (int k = 0; k < n; k++)
  {
    unsigned long long x = nextrand(), y = nextrand();

    {
      unsigned long long xx = x, yy = y, r1, r2;
      if (yy == 0)
        continue;
      r1 = xx / yy;
      r2 = xx % yy;
      acc += r1 * 3 + r2;
    }
    {
      unsigned int xx = (unsigned int)x, yy = (unsigned int)y, r1, r2;
      if (yy == 0)
        continue;
      r1 = xx / yy;
      r2 = xx % yy;
      acc += r1 * 5 + r2;
    }
    {
      unsigned short xx = (unsigned short)x, yy = (unsigned short)y, r1, r2;
      if (yy == 0)
        continue;
      r1 = xx / yy;
      r2 = xx % yy;
      acc += r1 * 7 + r2;
    }
    {
      unsigned char xx = (unsigned char)x, yy = (unsigned char)y, r1, r2;
      if (yy == 0)
        continue;
      r1 = xx / yy;
      r2 = xx % yy;
      acc += r1 * 11 + r2;
    }
  }
  return acc;
}

static int carried_over_continue(int n)
{
  int acc = 0;
  for (int i = 0; i < n; i++)
  {
    if (i % 3 == 0)
      continue; /* acc must survive the back-edge unchanged */
    acc = acc * 2 + i;
  }
  return acc;
}

static int arm_define_join(int n)
{
  int total = 0;
  for (int i = 0; i < n; i++)
  {
    int v;
    if (i & 1)
      v = i * 2;
    else
      v = i + 100;
    total += v; /* v is live-in at the merge, dead on the back-edge */
  }
  return total;
}

static long long loop_carried_i64(int n)
{
  long long acc = 1;
  for (int i = 0; i < n; i++)
  {
    if (i == 2)
      continue;
    acc = acc * 3 + i; /* 64-bit write: in place, not a fresh name */
  }
  return acc;
}

static int ptr_switch(int n)
{
  int a = 1, b = 2;
  int *p = &a;
  for (int i = 0; i < n; i++)
  {
    if (i & 1)
    {
      p = &b;
      continue; /* p carries to the next iteration */
    }
    *p = *p + i;
  }
  return a * 100 + b;
}

/* Irreducible CFG: the `goto` gives the loop a second entry, so it has no
 * single header and the live-in fixpoint has to converge on a multi-entry
 * region rather than a well-nested one. */
static int irreducible(int n, int start)
{
  int a = 1, b = 2, i = 0;
  if (start)
    goto mid;
  for (;;)
  {
    a = a + b;
    if (++i > n)
      break;
  mid:
    b = b + a;
    if (++i > n)
      break;
  }
  return a * 1000 + b;
}

static int live_out_of_loop(int n)
{
  int last = -1;
  for (int i = 0; i < n; i++)
  {
    if (i % 4 == 0)
      continue;
    last = i;
  }
  return last; /* read after the loop => live-in at the header */
}

int main(void)
{
  expect("divmod_scan(0)", (long long)divmod_scan(0), 0);
  expect("divmod_scan(40)", (long long)divmod_scan(40), 720413832877978715ll);
  expect("carried_over_continue(0)", carried_over_continue(0), 0);
  expect("carried_over_continue(11)", carried_over_continue(11), 286);
  expect("arm_define_join(0)", arm_define_join(0), 0);
  expect("arm_define_join(9)", arm_define_join(9), 552);
  expect("loop_carried_i64(0)", loop_carried_i64(0), 1);
  expect("loop_carried_i64(7)", loop_carried_i64(7), 948);
  expect("ptr_switch(0)", ptr_switch(0), 102);
  expect("ptr_switch(8)", ptr_switch(8), 114);
  expect("live_out_of_loop(0)", live_out_of_loop(0), -1);
  expect("live_out_of_loop(9)", live_out_of_loop(9), 7);
  expect("irreducible(0,0)", irreducible(0, 0), 3002);
  expect("irreducible(5,0)", irreducible(5, 0), 21034);
  expect("irreducible(0,1)", irreducible(0, 1), 1003);
  expect("irreducible(5,1)", irreducible(5, 1), 29018);
  printf("fails=%d\n", fails);
  return 0;
}
