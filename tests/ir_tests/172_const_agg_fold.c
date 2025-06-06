/* Regression guard for tcc_ir_opt_const_aggregate_fold (ir/opt_const_aggregate.c).
 *
 * Positive: a deterministic, unrolled double read-modify-write chain on a
 * non-escaping local union whose address is only used as a memmove SOURCE (the
 * by-value struct argument copy).  The pass should const-fold every `u.e.a++`
 * / `u.e.b--` (__aeabi_dadd/dsub) across the intervening calls.  The result
 * must still be correct.
 *
 * Negative: `clobber(&e)` escapes e's address to a non-memmove call, so the
 * pass must NOT treat e as a constant — the `+ 1.0` runs on the clobbered
 * value.  A wrong fold here would print the stale value.
 */
#include <stdio.h>

struct U { double a, b; };

__attribute__((noinline)) struct U passU(int x, struct U v) { (void)x; return v; }
__attribute__((noinline)) void clobber(double *p) { *p = 1000.0; }

int main(void)
{
  union Y { struct U e; } u, r;
  u.e.a = 1.25;
  u.e.b = 2.75;
  double acc = 0.0;

#define STEP(n)                                                  \
  do {                                                           \
    r.e = passU(n, u.e);                                         \
    if (u.e.a != r.e.a || u.e.b != r.e.b) { printf("FAIL%d\n", n); return 1; } \
    acc += u.e.a + u.e.b;                                        \
    u.e.a++;                                                     \
    u.e.b--;                                                     \
  } while (0)

  STEP(0);  /* 1.25 + 2.75 */
  STEP(1);  /* 2.25 + 1.75 */
  STEP(2);  /* 3.25 + 0.75 */
  /* acc = 4 + 4 + 4 = 12 */
  printf("acc=%.2f\n", acc);

  double e = 5.0;
  clobber(&e);     /* e := 1000.0 (escaped pointer) */
  e = e + 1.0;     /* must be 1001.0, NOT 6.0 */
  printf("e=%.2f\n", e);

  return (int)(acc + e); /* 12 + 1001 = 1013 */
}
