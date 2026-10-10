/* ssa:dce dead_overwrite_stores deleted a frame store that an inline asm
 * between it and the overwrite reads (asm input address, "memory" clobber). */
#include <stdio.h>
__attribute__((noinline)) void use(int *p) { printf("use %d\n", p[0]); }
__attribute__((noinline)) int as(void)
{
  int x[4];
  int r;
  x[0] = 1;
  __asm__ volatile("ldr %0, [%1]" : "=r"(r) : "r"(x) : "memory");
  x[0] = 2;
  use(x);
  return r;
}
int main(void)
{
  printf("as=%d\n", as());
  return 0;
}
