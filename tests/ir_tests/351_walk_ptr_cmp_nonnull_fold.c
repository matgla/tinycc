/* stack_addr_nonnull_fold must NOT fold a loop-exit comparison of a WALKING
 * stack pointer against a base address.  In `while (base != p) { *--p = v; ... }`
 * p is decremented each iteration until it reaches base: the two addresses are
 * distinct on the first iteration but become equal, so folding "distinct stack
 * addresses are never equal" drops the loop exit -> the loop never terminates
 * (or the caller collapses under DCE).
 *
 * Reproduces gcc.c-torture/execute/990513-1 as a self-contained pin.  The legacy
 * pre-propagation re-roller reshaped the body and masked the fold; with reroll
 * relocated to ssa:reroll (post-propagation) the walking-pointer loop reaches
 * the fold intact.  Fixed by declining the address-vs-address distinctness fold
 * for a CMP inside a loop body (in_loop guard in tcc_ir_opt_stack_addr_nonnull_fold).
 * `fill` inlines into main so the fold path is reached; vv is volatile so the
 * result cannot be const-folded, forcing the stores to actually run.
 * See docs/plan_legacy_loop_reroll_ssa.md.
 *
 * Reference (arm-none-eabi-gcc -O2): sum=448, matching tcc -O0/-O1/-O2/-Os. */
#include <stdio.h>
#include <string.h>

volatile int vv = 7;

static void fill(int *p, int n, int v)
{
  int *base = p;
  p += n;
  while (base != p)
    {
      *--p = v;
      *--p = v;
      *--p = v;
      *--p = v;
    }
}

int main(void)
{
  int a[64];
  memset(a, 0, sizeof(a));
  fill(a, 64, vv);
  long s = 0;
  for (int i = 0; i < 64; i++)
    s += a[i];
  printf("sum=%ld\n", s);
  return 0;
}
