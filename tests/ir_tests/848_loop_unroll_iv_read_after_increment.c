/* loop_unroll gave every IV read in copy k the value init + k*step, also the
 * reads that follow the increment in the body. */
#include <stdio.h>
int a[16];
__attribute__((noinline)) void f1(void) { int i = 0; while (i < 4) { i++; a[i] = i * 3; } }
__attribute__((noinline)) void f2(void) { int i = 0; while (i < 3) { a[i + 8] = i; i += 2; a[i + 8] = i * 5; } }
int main(void)
{
  f1();
  for (int k = 0; k < 6; k++) printf("%d ", a[k]);
  printf("\n");
  f2();
  for (int k = 8; k < 14; k++) printf("%d ", a[k]);
  printf("\n");
  return 0;
}
