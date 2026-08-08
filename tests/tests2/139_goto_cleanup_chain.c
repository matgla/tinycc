/* A `goto` out of a scope that has a __attribute__((cleanup)) variable starts
 * its own jump chain.  The chain sentinel is -1; a 0 there means "instruction
 * 0", so the label's backpatch walks into the first instruction of the
 * function and retargets it.  That is invisible while instruction 0 is not a
 * jump, but at -O1+ a leading `if (const_fn())` folds to an unconditional JMP
 * and gets silently repointed at the label -- skipping the whole function body
 * up to it. */

#include <stdio.h>

static int steps;

static void cl(int *p) { (void)p; }

/* Folds to a compile-time constant at -O1+, so the `if` below becomes the
 * function's very first instruction: an unconditional JMP. */
static int disabled(void) { return 0; }

static int f(int n)
{
  if (disabled())
    return -1;
  int __attribute__((cleanup(cl))) guard = 0;
  (void)guard;
  steps += 1;
  if (n > 3)
    goto save;
  steps += 10;
save:
  return steps;
}

int main(void)
{
  printf("%d\n", f(0));
  steps = 0;
  printf("%d\n", f(9));
  return 0;
}
