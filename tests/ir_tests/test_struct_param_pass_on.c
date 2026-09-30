/* A by-value struct parameter passed on by value to another call.  Large ones
 * are copied into the outgoing area by memcpy, around which the caller saves
 * r0-r3, IP and LR -- the source address of a parameter must account for that
 * push just as a local's does.  Covers a struct split across r1-r3 and the
 * stack, one wholly on the stack, and small ones in registers. */
#include <stdio.h>

struct Big { int v[40]; };
struct Mid { int v[6]; };

__attribute__((noinline)) static int sum_big(int k, struct Big b)
{
  int r = k;
  for (int i = 0; i < 40; i++)
    r += b.v[i] * (i + 1);
  return r;
}

__attribute__((noinline)) static int sum_mid(struct Mid m, int k)
{
  int r = k;
  for (int i = 0; i < 6; i++)
    r += m.v[i] * (i + 3);
  return r;
}

/* b arrives split across r1-r3 and the stack. */
__attribute__((noinline)) static int split_on(int k, struct Big b) { return sum_big(k + 1, b) + b.v[39]; }

/* b arrives wholly on the stack. */
__attribute__((noinline)) static int stack_on(int a, int c, int d, int e, struct Big b, struct Mid m)
{
  return sum_big(a + c + d + e, b) + sum_mid(m, b.v[0]);
}

int main(void)
{
  struct Big b;
  struct Mid m;
  for (int i = 0; i < 40; i++)
    b.v[i] = i * 7 - 3;
  for (int i = 0; i < 6; i++)
    m.v[i] = 100 - i;
  printf("%d\n", split_on(5, b));
  printf("%d\n", stack_on(1, 2, 3, 4, b, m));
  return 0;
}
