/* canonicalize_zig_counter moved the counter init `B = #0` onto the counter
 * A without checking that A is defined between the init and the loop: the
 * load `a = *p` then overwrote the moved init and the loop started from 100
 * instead of 0. */
#include <stdio.h>
int sum;
void __attribute__((noinline)) use(int v) { sum += v; }
void __attribute__((noinline)) f(int *out, int *p)
{
  int a, b;
  b = 0;
  a = *p;
  out[0] = a;
  while (1) {
    a = b;
    if (!(a < 1000)) break;
    use(a);
    a = a + 1;
    b = a;
  }
}
int main(void)
{
  int out[2] = {-1, -1}, v = 100;
  f(out, &v);
  printf("%d %d\n", out[0], sum);
  return 0;
}
