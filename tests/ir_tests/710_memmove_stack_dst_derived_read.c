#include <stdio.h>

/* memmove_to_indexed_stores, stack destination: the relocated stores wrote
 * r.p before `t.v[0] += r.p.v[2]` read it through an address derived from
 * &r (a LOAD_INDEXED off Addr[r], which the old scan skipped as "only an
 * address").  Without that fold, HEAD at 3c2dd5e4 also faulted here (a
 * dropped base address), fixed by the frame-reduction work. */
typedef struct { int v[5]; } P;
static P mkp(int k) { P p; for (int i = 0; i < 5; i++) p.v[i] = k + i; return p; }

__attribute__((noinline)) int stack_dst(void)
{
  struct { int err; P p; } r;
  r.p = mkp(1);
  P t = mkp(4);
  t.v[0] += r.p.v[2];
  r.p = t;
  return r.p.v[0];
}

int main(void)
{
  printf("%d\n", stack_dst());
  return 0;
}
