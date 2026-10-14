/* compute_trip_count evaluated unsigned exit conditions in signed arithmetic:
 * a loop starting at 0xFFFFFFFE with `i < 3u` (zero trips) was unrolled or
 * simulated as 5 trips. */
#include <stdio.h>
int a[16];
__attribute__((noinline)) int u1(void) { int s = 0; for (unsigned i = 0xFFFFFFFEu; i < 3u; i++) s += 5; return s; }
__attribute__((noinline)) void u2(void) { for (unsigned i = 0xFFFFFFFEu; i < 3u; i++) a[i & 15] += 5; }
__attribute__((noinline)) int u3(void) { int s = 0; unsigned i = 0xFFFFFFFEu; do { s += 5; a[i & 15]++; i++; } while (i < 3u); return s; }
__attribute__((noinline)) int u4(void) { int s = 0; for (unsigned i = 0xFFFFFFFEu; i <= 3u; i++) s += 5; return s; }
__attribute__((noinline)) int u5(void) { int s = 0; for (unsigned i = 1; i < 4u; i++) s += 5; return s; }
int main(void)
{
  printf("u1 %d\n", u1());
  u2();
  printf("u2 %d %d\n", a[14], a[0]);
  int r = u3();
  printf("u3 %d %d %d\n", r, a[14], a[15]);
  printf("u4 %d\n", u4());
  printf("u5 %d\n", u5());
  return 0;
}
