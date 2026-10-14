/* An alignment pad before a loop head may widen the 16-bit instruction in
 * front of it into its 32-bit form instead of executing a NOP -- but only when
 * nothing branches to the pad's address.  `if (c) i = K;` leaves a code-free
 * join op mapped exactly there (the phi copy is coalesced): the `cbz c` skips
 * `movs i, #K` and lands on the pad.  With the movs widened to `movs.w`, the
 * c == 0 path started executing the second halfword 0x02KK, i.e.
 * `lsls r(K&7), r((K>>3)&7), #8`, clobbering x (K = 9: lsls r1, r1, #8) or
 * the base pointer.  The pad must stay a NOP there. */
#include <stdio.h>

int a[64];
volatile int v0 = 0, v1 = 1, vx = 1000;

#define F(N, K)                                                                                                        \
  __attribute__((noinline)) int f##N(int c, int x)                                                                     \
  {                                                                                                                    \
    int i = 0;                                                                                                         \
    if (c)                                                                                                             \
      i = K;                                                                                                           \
    int s = x;                                                                                                         \
    for (; i < 40; i++)                                                                                                \
      s += i ^ x;                                                                                                      \
    return s;                                                                                                          \
  }                                                                                                                    \
  __attribute__((noinline)) void g##N(int c, int x)                                                                    \
  {                                                                                                                    \
    int i = 0;                                                                                                         \
    if (c)                                                                                                             \
      i = K;                                                                                                           \
    for (; i < 40; i++)                                                                                                \
      a[i] = i + x;                                                                                                    \
  }

F(1, 1)
F(2, 2)
F(3, 3)
F(4, 8)
F(5, 9)
F(6, 10)
F(7, 11)
F(8, 16)
F(9, 17)
F(10, 25)
F(11, 33)

static int suma(void)
{
  int s = 0;
  for (int i = 0; i < 40; i++)
    s += a[i];
  return s;
}

#define T(N)                                                                                                           \
  do                                                                                                                   \
  {                                                                                                                    \
    printf("t%d", N);                                                                                                  \
    for (int c = 0; c < 2; c++)                                                                                        \
    {                                                                                                                  \
      int r = f##N(c ? v1 : v0, vx);                                                                                   \
      for (int j = 0; j < 64; j++)                                                                                     \
        a[j] = 0;                                                                                                      \
      g##N(c ? v1 : v0, vx);                                                                                           \
      printf(" %d/%d", r, suma());                                                                                     \
    }                                                                                                                  \
    printf("\n");                                                                                                      \
  } while (0)

int main(void)
{
  T(1);
  T(2);
  T(3);
  T(4);
  T(5);
  T(6);
  T(7);
  T(8);
  T(9);
  T(10);
  T(11);
  return 0;
}
