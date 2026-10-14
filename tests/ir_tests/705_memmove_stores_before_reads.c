#include <stdio.h>

/* memmove_to_indexed_stores retargets the stores that built a temporary
 * onto the copy's destination, at their own (earlier) positions.  Reads of
 * the destination in between through the pointer itself then saw the new
 * values.  (The stack-destination half is 710.) */
typedef struct { int a[6]; } S;
static inline S sw(S *p) { S r = {{0}}; r.a[0] = p->a[1]; r.a[1] = p->a[0]; return r; }
__attribute__((noinline)) void t(S *q) { *q = sw(q); }

int main(void)
{
  S y = {{7, 8, 9, 1, 1, 1}};
  t(&y);
  printf("%d %d %d\n", y.a[0], y.a[1], y.a[2]);
  return 0;
}
