/* Regression guard for the loop-carried store back-edge fix in
 * ir/opt_dead_lea_store.c (tcc_ir_opt_dead_lea_store_elim).
 *
 * A store to a LEA'd local inside a loop, whose slot is read at the loop top on
 * the next iteration, is loop-carried-live.  The pass's position-based liveness
 * (`read.pos > store.pos`) missed the back-edge and wrongly eliminated the
 * write-back — e.g. `while (c.v-- > 0)` never terminated correctly (returned 101
 * instead of the right count).  Manifested at -O1 (was OK at -O0/-O2), so these
 * MUST be exercised at every optimization level.
 */
#include <stdio.h>

/* Plain int field decremented in a loop (the minimal repro). */
struct ctr { int v; };
int countdown_int(int start)
{
  struct ctr c;
  c.v = start;
  int n = 0;
  while (c.v-- > 0)
  {
    n++;
    if (n > 1000)
      break;
  }
  return n;
}

/* Bitfield field decremented in a loop. */
struct bits { unsigned int b : 3, pad : 5; };
int countdown_bf(void)
{
  struct bits s = {5, 0};
  int n = 0;
  while (s.b-- > 0)
  {
    n++;
    if (n > 1000)
      break;
  }
  return n;
}

/* Loop-carried accumulate into a MEMORY local (struct field / array element):
 * the post-loop read must NOT be SCCP-forwarded to the entry init store across
 * the accumulate loop.  Was a distinct -O1 miscompile (returned 0). */
struct acc { int sum; };
int sum_field(int k)
{
  struct acc a;
  a.sum = 0;
  for (int i = 1; i <= k; i++)
    a.sum += i;
  return a.sum;
}
int sum_arr(int k)
{
  int s[1];
  s[0] = 0;
  for (int i = 1; i <= k; i++)
    s[0] += i;
  return s[0];
}

/* Accumulate into a stack local through a CALL that receives the slot's
 * ADDRESS by-reference.  Two shapes that must NOT SCCP-forward the entry init
 * store across the loop:
 *
 *  - bump_extern: a true external call (callee opaque) — caught by the
 *    loop-FUNCPARAM by-ref-address check.
 *  - bump_static: a same-TU static helper that the inliner expands in place to
 *    `*p = *p + 2`; the pointer flows through the inlined param V-register, so
 *    the store's stack offset is unresolvable.  Such an opaque pointer-deref
 *    write inside the loop may alias the slot and must block the forward.
 * Both wrongly returned 0 at -O1 before the fix. */
struct box { int x; };
static void bump_static(struct box *p) { p->x += 2; }
int via_static_call(int k)
{
  struct box b;
  b.x = 0;
  for (int i = 0; i < k; i++)
    bump_static(&b);
  return b.x;
}

int main(void)
{
  int ok = 1;
  if (countdown_int(3) != 3) ok = 0;
  if (countdown_int(0) != 0) ok = 0;
  if (countdown_int(10) != 10) ok = 0;
  if (countdown_bf() != 5) ok = 0;      /* 5,4,3,2,1 each >0 → 5 iters */
  if (sum_field(5) != 15) ok = 0;       /* 1+2+3+4+5 */
  if (sum_field(0) != 0) ok = 0;
  if (sum_arr(5) != 15) ok = 0;
  if (via_static_call(5) != 10) ok = 0; /* 5 iters * +2 */
  if (via_static_call(0) != 0) ok = 0;
  printf("%s\n", ok ? "OK" : "FAIL");
  return 0;
}
