/* deref_operand_cse / global_deref_cse: the region flush at an entry point must
 * happen even when the instruction sitting at that index is a NOP.
 *
 * Two `if`s over fields of the same `cfg->blocks[b]` give the pass two reads of
 * `cfg->blocks` to fold together.  The second `if`'s block is a join: the first
 * `if`'s taken arm branches to it, and by the time the pass runs that target
 * index holds a NOP (the else arm was blanked).  Skipping NOPs before the
 * entry-point check let the region span the join, so the merge block read a CSE
 * temp that only the fall-through predecessor had defined -- yielding
 * `blocks + 2*b*sizeof(B)` instead of `blocks + b*sizeof(B)`.
 *
 * This is verbatim the eff_start/eff_end scan of lcs_try_candidate in
 * source/opt/ssa/loop/loop_const_sim.c, which is why the self-hosted compiler
 * folded away loops that should have run. */
#include <stdio.h>

struct B { int s; int e; int pad[14]; };
struct C { struct B *blocks; int n; };

/* noinline via a global: keep the loop in its own function so the shape above
 * survives to the pass. */
struct C *g_cfg;

int span(struct C *cfg, unsigned char *member)
{
  int lo = 1000, hi = -1;
  for (int b = 0; b < cfg->n; b++) {
    if (!member[b])
      continue;
    if (cfg->blocks[b].s < lo)
      lo = cfg->blocks[b].s;
    if (cfg->blocks[b].e - 1 > hi)
      hi = cfg->blocks[b].e - 1;
  }
  return lo * 1000 + hi;
}

int main(void)
{
  /* The data matters, in two ways.
   *
   * (1) The faulty address is only read on the path where the FIRST `if` did
   *     not fire, so at least one selected block must have `s >= lo`.  With
   *     strictly decreasing `s` every block takes the other arm and the bug is
   *     invisible.  Block 2 is that one, and the stale base makes it read
   *     blocks[2*2] instead of blocks[2].
   *
   * (2) The poisoned read feeds only the `> hi` COMPARISON -- the value finally
   *     stored is fetched again through a correctly recomputed address.  So the
   *     bug has to flip the decision: blocks[4].e is small enough to lose the
   *     comparison while blocks[2].e would have won it, and `hi` is left at the
   *     previous block's value instead of being updated.
   */
  static struct B blocks[5];
  static unsigned char member[5] = { 0, 1, 1, 0, 0 };
  struct C cfg;
  static const int svals[5] = { 100,  80,  90,  70,  60 };
  static const int evals[5] = { 700, 300, 500, 209, 100 };

  for (int i = 0; i < 5; i++) {
    blocks[i].s = svals[i];
    blocks[i].e = evals[i];
  }
  cfg.blocks = blocks;
  cfg.n = 5;
  g_cfg = &cfg;

  /* lo = min(80, 90) = 80; hi = max(300, 500) - 1 = 499.
   * With the bug block 2 compares against blocks[4].e = 100, loses, and hi is
   * left at blocks[1].e - 1 = 299. */
  printf("span = %d\n", span(&cfg, member));

  /* No member selected: the guards never fire, so the initial values survive. */
  static unsigned char none[5] = { 0, 0, 0, 0, 0 };
  printf("empty = %d\n", span(&cfg, none));
  return 0;
}
