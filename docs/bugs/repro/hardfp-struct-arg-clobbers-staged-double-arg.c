/* docs/bugs/hardfp-struct-arg-clobbers-staged-double-arg.md
 * -mfloat-abi=hard: a by-value struct argument in r0-r2 and a double argument
 * that is itself a call result.  The double is staged in r0:r1, the struct's
 * LDM then overwrites r0-r2, and `vmov d0, r0, r1` passes struct words.
 * Expected (gcc): "8"   tcc -mfloat-abi=hard, every level: "5". */
#include <stdio.h>
struct S { int a, b, c; };
__attribute__((noinline)) int arg(int n) { return n; }
__attribute__((noinline)) double dbl(double d) { return d; }
__attribute__((noinline)) int ks(struct S s, int c, double f) { return s.a + s.b + s.c + c + (int)(f * 2); }
int main(void)
{
  struct S s = {1, 2, 3};
  printf("%d\n", ks(s, arg(-1), dbl(1.5)));
  return 0;
}
