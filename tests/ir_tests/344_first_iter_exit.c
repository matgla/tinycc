/* First-iteration-exit peeling regression
   (docs/plan_legacy_loop_dead_first_iter_ssa.md).

   Pins the behavior of the first-iteration-exit loop elimination
   (legacy tcc_ir_opt_loop_dead_first_iter, replaced by
   ssa:first_iter_exit) across the pre-SSA -> SSA migration:

   1. the 20070824-1.c pointer-chase shape: `for (p = &s; *p; p = &(*p)->a);`
      with s == NULL provably exits on iteration 1.  The loop may be
      eliminated, but the post-loop pointer surgery must still run: the
      store through the exit value of p must reach s, and the post-loop
      `if (!s)` must see the updated value.
   2. a runtime-trip-count control loop that must NOT be eliminated.
   3. the same chase over a one-element list: the loop body must execute
      exactly once before the exit test goes false. */
#include <stdio.h>

struct S
{
  struct S *a;
  int b;
};

volatile int vten = 10;

int main(void)
{
  /* shape 1: chase over the empty list — provably exits on iteration 1 */
  struct S *s = (struct S *) 0, **p, *n;
  for (p = &s; *p; p = &(*p)->a)
    ;
  n = (struct S *) __builtin_alloca(sizeof(*n));
  n->a = *p;
  n->b = 42;
  *p = n;
  if (!s)
    return 1; /* must not happen: `*p = n` wrote s */
  printf("chase b=%d null_a=%d\n", s->b, s->a == 0);

  /* shape 2: runtime trip count — must survive */
  int i = 0, sum = 0;
  while (i < vten) {
    sum += i;
    i++;
  }
  printf("sum=%d\n", sum);

  /* shape 3: chase over a one-element list — exactly one iteration */
  struct S first, node;
  first.a = 0;
  first.b = 7;
  node.a = 0;
  node.b = 8;
  s = &first;
  for (p = &s; *p; p = &(*p)->a)
    ;
  *p = &node;
  printf("tail b=%d\n", first.a->b);
  return 0;
}
