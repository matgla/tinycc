#include <stdio.h>

/* global_base_share clusters stores to one global behind a shared base vreg.
 * A join point that lands on a NOP did not end the cluster, so the path that
 * skipped the base's definition stored through garbage. */
typedef struct { unsigned a[8]; } B;
static unsigned acc;
__attribute__((noinline)) B mk(unsigned k) { B r; for (int i = 0; i < 8; i++) r.a[i] = k + i; return r; }
__attribute__((noinline)) void h1(unsigned k, unsigned *p)
{
  if (k & 2) {
    if (k & 1)
      acc += p[2];
    B l5 = mk(236u);
    acc += p[4];
  }
}

int main(void)
{
  unsigned Z[8];
  for (int i = 0; i < 8; i++) Z[i] = 7u + i;
  h1(38u, Z);
  printf("acc %u\n", acc);
  h1(39u, Z);
  printf("acc %u\n", acc);
  return 0;
}
