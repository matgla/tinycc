/* ssa:sccp treated an escaped frame address as reaching only 4096 bytes, so a
 * call taking the base of a larger local array was assumed not to write
 * buf[4200]; the load after the call folded to the sentinel stored before it. */
#include <stdio.h>

__attribute__((noinline)) void wr2(char *p) { p[4200] = 7; }

__attribute__((noinline)) int g(void)
{
  char buf[4400];
  buf[4200] = 1;
  wr2(buf);
  return buf[4200];
}

int main(void)
{
  printf("buf[4200]=%d\n", g());
  return 0;
}
