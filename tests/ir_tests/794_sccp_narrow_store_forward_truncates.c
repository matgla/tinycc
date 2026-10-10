/* ssa:sccp forwarded the full-width source of an 8/16-bit frame store to the
 * narrow load without truncating / re-extending it. */
#include <stdio.h>

__attribute__((noinline)) int g(int c)
{
  signed char b[4];
  int k = 200;
  if (c)
    k = 200;
  b[1] = k + c * 0;
  return b[1];
}

__attribute__((noinline)) int h(int c)
{
  short b[4];
  int k = 70000;
  if (c > 5)
    k = 70000;
  b[2] = k;
  return b[2];
}

__attribute__((noinline)) int u(int c)
{
  unsigned char b[4];
  int k = -3;
  if (c)
    k = -3;
  b[3] = k;
  return b[3];
}

__attribute__((noinline)) int us(int c)
{
  unsigned short b[4];
  int k = -3;
  if (c > 5)
    k = -3;
  b[0] = k;
  return b[0];
}

int main(void)
{
  printf("%d %d %d %d\n", g(1), h(1), u(1), us(1));
  return 0;
}
