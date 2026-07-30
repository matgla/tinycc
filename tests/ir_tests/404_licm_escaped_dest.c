/* Guard: ssa:licm must not hoist a loop-invariant def whose destination is an
 * address-taken local that the loop can redefine through the escaped address.
 *
 * `v = 5` looks invariant — one visible def in the loop, none outside — but
 * `g(&v)` writes through the escaped pointer, so the assignment has to happen
 * once per iteration.  Hoisting it into the preheader let g's side effect
 * accumulate across iterations (acc came out 21 instead of 18).
 *
 * The read-side of this rule (an operand whose address escapes is not
 * invariant) was already guarded; this is the write-side mirror.  The va_start
 * intrinsic hits the exact same shape, hence `restart_each_iteration` below:
 * a per-iteration reset of a pointer that a callee then advances.
 */
#include <stdio.h>

int calls;

__attribute__((noinline)) void bump(int *p) { *p += 1; calls++; }

__attribute__((noinline)) int invariant_store_escapes(int n)
{
  int v;
  int acc = 0;
  while (n-- > 0)
  {
    v = 5;    /* invariant-looking, but bump() redefines it below */
    bump(&v);
    acc += v; /* 6 every iteration */
  }
  return acc;
}

/* Same shape with a pointer: the callee advances *pp, the loop resets it. */
__attribute__((noinline)) void advance(int **pp) { *pp += 1; }

__attribute__((noinline)) int restart_each_iteration(int n)
{
  static int data[8] = {10, 20, 30, 40, 50, 60, 70, 80};
  int *p;
  int acc = 0;
  while (n-- > 0)
  {
    p = data;   /* reset every iteration */
    advance(&p);
    acc += *p;  /* always data[1] == 20 */
  }
  return acc;
}

/* A genuinely invariant def of a NON-escaping local must still be hoistable —
 * this is the case the guard must not pessimize. */
__attribute__((noinline)) int invariant_store_no_escape(int n)
{
  int v;
  int acc = 0;
  while (n-- > 0)
  {
    v = 5;
    acc += v;
  }
  return acc;
}

int main(void)
{
  printf("escaped=%d calls=%d\n", invariant_store_escapes(3), calls);
  printf("restart=%d\n", restart_each_iteration(4));
  printf("noescape=%d\n", invariant_store_no_escape(3));
  return 0;
}
