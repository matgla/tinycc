#include <stdio.h>
typedef int v4 __attribute__((vector_size(16)));
__attribute__((noinline)) double ra(int z, _Complex double a, _Complex double b) { return __real__ a * 100 + __real__ b; }
__attribute__((noinline)) int va(int z, v4 a, v4 b) { return a[0] * 100 + b[0]; }
__attribute__((noinline)) double run(_Complex double x, _Complex double y) { return ra(0, x + x, y + y); }
__attribute__((noinline)) int runv(v4 x, v4 y) { return va(0, x + x, y + y); }
__attribute__((noinline)) double call(void) { return ra(0, 1.0 + 2.0i, 3.0 + 4.0i); }
int main(void) {
  v4 p = {1, 2, 3, 4}, q = {3, 3, 3, 3};
  printf("run=%g (206) runv=%d (206) call=%g (103)\n", run(1.0 + 0i, 3.0 + 0i), runv(p, q), call());
  return 0;
}
