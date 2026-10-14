/* loop_eliminate's try_eliminate_loop_symbolic rewrote every counting loop
 * with a symbolic limit as the closed form of `i < n` (n*K gated by n > 0),
 * ignoring the exit condition: `i <= n` lost its last iteration and the
 * never-entered `i > n` / `i >= n` loops came out as n*K. */
#include <stdio.h>
volatile int vn = 5;
__attribute__((noinline)) int lt(int n) { int acc = 0; for (int i = 0; i < n; i++) acc += 3; return acc; }
__attribute__((noinline)) int le(int n) { int acc = 0; for (int i = 0; i <= n; i++) acc += 3; return acc; }
__attribute__((noinline)) int gt(int n) { int acc = 0; for (int i = 0; i > n; i++) acc += 3; return acc; }
__attribute__((noinline)) int ge(int n) { int acc = 0; for (int i = 0; i >= n; i++) acc += 3; return acc; }
int main(void)
{
  int n = vn;
  printf("lt %d %d %d\n", lt(n), lt(0), lt(-4));
  printf("le %d %d %d\n", le(n), le(0), le(-4));
  printf("gt %d\n", gt(n));
  printf("ge %d\n", ge(n));
  return 0;
}
