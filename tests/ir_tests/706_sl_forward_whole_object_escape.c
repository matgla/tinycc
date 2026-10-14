#include <stdio.h>
#include <string.h>

/* sl_forward kept stores to a local across a call that received pointers
 * into it, bounding the callee's reach by what the caller itself touched;
 * and an asm output into a slot was not a write at all. */
typedef struct { int a[4]; } Q;
__attribute__((noinline)) int d1(int k) { Q s = {{k, k + 1, k + 2, k + 3}}; memmove(&s.a[1], &s.a[0], 12); return s.a[2]; }
static void wr(int *d, int *s) { d[2] = s[0] + 70; }
void (*volatile fp)(int *, int *) = wr;
__attribute__((noinline)) int e1(int k) { Q s = {{k, k + 1, k + 2, k + 3}}; fp(&s.a[1], &s.a[0]); return s.a[3]; }
__attribute__((noinline)) int use(int *p) { return p[0]; }
__attribute__((noinline)) int a1(int v)
{
  int arr[4];
  int big[8];
  arr[0] = 1;
  arr[1] = v;
  big[0] = v;
  int r = use(big);
  asm volatile("adds %0, %0, #2" : "+r"(arr[1]) : : "cc");
  r += use(big);
  return r + arr[1] * 100 + arr[0];
}

int main(void)
{
  printf("%d %d %d\n", d1(1), e1(1), a1(5));
  return 0;
}
