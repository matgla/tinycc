/* ssa:reroll — identical-block re-rolling relocated to the post-propagation
 * regalloc flat region (docs/plan_legacy_loop_reroll_ssa.md).
 *
 * Two pins in one program:
 *
 *  A) Non-foldable opaque-call repetition SURVIVES propagation and re-rolls.
 *     `step(1,2)` (period 3: PARAM, PARAM, CALL) repeated 6x accumulates into a
 *     global, so downstream const-prop cannot collapse it; ssa:reroll turns the
 *     run into a counted loop calling step 6 times.  Correctness is invariant to
 *     whether the pass fires: acc and ncalls must be exact either way (this is
 *     the fuzz-sensitive call-rerolling path, cf. 299).
 *
 *  B) Foldable macro-unrolled repetition is NOT hurt: 8 identical increments of
 *     a plain accumulator fold to a constant downstream (the legacy pre-prop
 *     re-roller used to inhibit this fold; the post-prop placement lets it win).
 *
 * step() is noinline so the calls survive to the re-roller.
 *
 * Reference (arm-none-eabi-gcc -O2): acc=612 ncalls=6 folded=8, matching
 * tcc -O0/-O1/-O2/-Os. */
#include <stdio.h>

static long acc = 0;
static int ncalls = 0;

__attribute__((noinline)) static void step(int a, int b)
{
  acc += (long)a * 100 + b;
  ncalls++;
}

int main(void)
{
  /* A: period-3 opaque-call run, 6 repeats -> re-rollable, non-foldable. */
  step(1, 2); step(1, 2); step(1, 2);
  step(1, 2); step(1, 2); step(1, 2);

  /* B: foldable macro-unrolled accumulator (collapses to +8 downstream). */
  int folded = 0;
  folded += 1; folded += 1; folded += 1; folded += 1;
  folded += 1; folded += 1; folded += 1; folded += 1;

  printf("acc=%ld ncalls=%d folded=%d\n", acc, ncalls, folded);
  return 0;
}
