/* VRP must not evaluate a LOOP-CARRIED phi operand with ranges scoped to the
 * CURRENT iteration.  `prev` is last iteration's `t`; inside `if (t >= 256)`
 * the guard narrows THIS iteration's t, but says nothing about prev.
 * sv_phi_excludes_const resolved the backedge operand through the in-scope
 * range and proved `prev == 5` impossible ({0} from entry, [256,..] from the
 * guard), folding the checks away.  The self-host casualty was tccgen's
 * prescan_captured_vars: its `if (prev_tok == TOK_LAND)` (and TOK_GOTO)
 * blocks vanished, so a nested function taking `&&parent_label` compiled to
 * "label used but not defined" (gcc-torture 920415-1, 920721-4, 20061220-1,
 * pr37669).  The same hazard is guarded in sv_backprop_phi_eq and the
 * sv_seed_phis hull.
 */
#include <stdio.h>

int data[] = {5, 300, 7, 301, 5, 302, 0};

int main(void)
{
  int prev = 0;
  int hits = 0;
  for (int i = 0; data[i]; i++)
  {
    int t = data[i];
    if (t >= 256)
    {
      if (prev == 5)
        hits += 1;
      if (prev == 7)
        hits += 10;
    }
    prev = t;
  }
  printf("hits=%d\n", hits);
  if (hits == 12)
    printf("OK\n");
  else
    printf("FAIL\n");
  return 0;
}
