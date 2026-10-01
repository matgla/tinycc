/* A struct argument's stack part of three to seven words is copied by
 * LDM/STM when enough registers are free: at the bottom of the outgoing area
 * (stmia sp) and above it (a second base register), from locals, with other
 * values live across the call setup. */
#include <stdio.h>

struct W6 { int a[6]; };
struct W7 { int a[7]; };
struct W9 { int a[9]; };
struct W10 { int a[10]; };

__attribute__((noinline)) static int h9(int k, struct W9 s)
{
  int r = k;
  for (int i = 0; i < 9; i++)
    r = r * 5 + s.a[i];
  return r;
}
__attribute__((noinline)) static int h10(struct W10 s, int k)
{
  int r = k;
  for (int i = 0; i < 10; i++)
    r = r * 7 + s.a[i];
  return r;
}
/* Two struct arguments: the second one's stack part sits above the first's. */
__attribute__((noinline)) static int h67(int a, int b, int c, int d, struct W6 x, struct W7 y)
{
  int r = a + b * 2 + c * 3 + d * 4;
  for (int i = 0; i < 6; i++)
    r = r * 3 + x.a[i];
  for (int i = 0; i < 7; i++)
    r = r * 11 + y.a[i];
  return r;
}

__attribute__((noinline)) static int drive(int k)
{
  struct W9 s9;
  struct W10 s10;
  struct W6 s6;
  struct W7 s7;
  for (int i = 0; i < 10; i++)
  {
    if (i < 9)
      s9.a[i] = k * i + 1;
    s10.a[i] = k - i;
    if (i < 6)
      s6.a[i] = i * i + k;
    if (i < 7)
      s7.a[i] = k ^ i;
  }
  int live1 = k * 3, live2 = k * 5 + 1;
  int r = h9(k, s9);
  r ^= h10(s10, live1);
  r += h67(live1, live2, k, k + 1, s6, s7);
  return r + live1 * live2;
}

int main(void)
{
  printf("%d %d\n", drive(1), drive(1234));
  return 0;
}
