/* The loop passes took the first `V <- #imm` within a few instructions of the
 * preheader as the IV's entry value without checking that it reaches the loop:
 * a conditional init, or one overwritten by a call, gave a wrong trip count,
 * and unroll deleted an init that was still read. */
#include <stdio.h>
volatile int vc = 1, v0 = 0;
int a[16];
int calls;
__attribute__((noinline)) int g(int c) { return c + 1; }
__attribute__((noinline)) void h(void) { calls++; }
__attribute__((noinline)) int f2(int c) { int i = 0; if (c) i = 2; int s = 0; for (; i < 6; i++) s += i * 7; return s; }
__attribute__((noinline)) void f2c(int c) { int i = 0; if (c) i = 2; for (; i < 4; i++) a[i] = i + 10; }
__attribute__((noinline)) int f2d(int c) { int i = 1; int s = 0; s = i; i = g(c); for (; i < 6; i++) s += i * 7; return s; }
/* the init is also read before the loop: it must not be deleted */
__attribute__((noinline)) int f2e(int c) { int i = 1; int s = i + c; for (; i < 4; i++) a[i + 8] = i + s; return s; }
__attribute__((noinline)) void d3(int c) { int i = 0; if (c) i = 12; for (; i < 10; i++) h(); }
int main(void)
{
  printf("f2 %d %d\n", f2(v0), f2(vc));
  f2c(v0);
  for (int k = 0; k < 5; k++) printf("%d ", a[k]);
  printf("\n");
  printf("f2d %d\n", f2d(v0));
  int e = f2e(v0);
  printf("f2e %d %d %d %d\n", e, a[9], a[10], a[11]);
  d3(v0); printf("d3 %d\n", calls);
  d3(vc); printf("d3 %d\n", calls);
  return 0;
}
