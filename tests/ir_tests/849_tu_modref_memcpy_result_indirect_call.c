/* TU dead-static / mod-ref: memcpy's returned pointer reads the static, an
 * indirect call may write any global, __attribute__((used)) statics stay. */
#include <stdio.h>
#include <string.h>
static int s[8];
__attribute__((noinline)) void put(int v) { s[7] = v; }
__attribute__((noinline)) int take(const int *src, unsigned n) { int *p = memcpy(s, src, n); return p[7]; }

struct S { int a, b, c, d, e; };
struct S GS;
void (*fp)(void);
static void bump(void) { GS.e++; }
__attribute__((noinline)) static void helper(void) { fp(); }
__attribute__((noinline)) int test(void) {
  struct S x = GS; helper();
  if (x.e != GS.e) return 1;
  return 0;
}
/* A `used` static may be read by code the compiler cannot see (here: asm). */
static int xu __attribute__((used));
__attribute__((noinline)) void setu(int v) { xu = v; }
__attribute__((noinline)) int readu(void)
{
  int r;
  __asm__ volatile("ldr %0, 1f\n\tldr %0, [%0]\n\tb 2f\n\t.align 2\n1:\t.word xu\n2:" : "=r"(r));
  return r;
}

int main(void) {
  int src[8] = {0};
  put(42);
  printf("take %d\n", take(src, 4));
  fp = bump;
  printf("test %d\n", test());
  setu(7);
  printf("used %d\n", readu());
  return 0;
}
