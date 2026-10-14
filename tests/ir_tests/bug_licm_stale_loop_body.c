/* Loop body records went stale after an earlier hoist in a nested function.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* tcc_ir_detect_loops gives every loop's body_instrs as a window of one shared
 * ramp array, so the windows of nested loops overlap.  The LICM hoists shifted
 * those entries once per loop record, i.e. once per enclosing loop: after the
 * four field loads hoisted out of the first loop, the second loop's body (one
 * outer loop deep) pointed four instructions too far and missed
 * `p = &t->e[i]`.  licm_field_ptr then took `p` for a loop-invariant root and
 * read `p->key` once in front of the loop: the search compared one stale
 * element (`6 6` expected; the device tcc crashed in tcc_ir_ssa_opt_loop_addr_hoist
 * the same way). */
#include <stdio.h>
struct ent { int key, val, a, b, c; };
struct tab { struct ent *e; int n; int m1, m2, m3, m4, m5; };

__attribute__((noinline)) int count(struct tab *t, int reps, int key)
{
  int hits = 0, n = t->n;
  for (int r = 0; r < reps; r++)
  {
    int s = 0;
    for (int i = 0; i < n; i++)
      s += t->m1 + t->m2 + t->m3 + t->m4 + t->m5;
    hits += s & 1;
    int i;
    for (i = 0; i < n; i++)
      if (t->e[i].key == key)
        break;
    hits += i;
  }
  return hits;
}

int main(void)
{
  struct ent e[6] = {{1}, {2}, {3}, {4}, {5}, {6}};
  struct tab t = {e, 6, 1, 2, 3, 4, 5};
  printf("%d %d\n", count(&t, 2, 4), count(&t, 1, 9));
  return 0;
}
