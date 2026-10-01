/* Word-by-word copies between frame slots fuse into LDM/STM chunks using the
 * registers free at the copy (codegen block-copy peephole): 3 to 10 words,
 * with values live across the copy, a self-assignment, and a volatile
 * destination that must keep its plain stores. */
#include <stdio.h>

struct Q3 { int a[3]; };
struct Q5 { int a[5]; };
struct Q7 { int a[7]; };
struct Q10 { int a[10]; };

__attribute__((noinline)) static void touch(void *p, int n)
{
  int *q = p;
  for (int i = 0; i < n; i++)
    q[i] = q[i] * 3 + i;
}

#define COPY_TEST(T, N)                                                                                                \
  __attribute__((noinline)) static int copy_##T(int k)                                                               \
  {                                                                                                                    \
    struct T x, y;                                                                                                     \
    for (int i = 0; i < N; i++)                                                                                        \
      x.a[i] = k * i + 1;                                                                                              \
    touch(&x, N);                                                                                                      \
    int live1 = k * 7, live2 = x.a[0] + 1;                                                                             \
    y = x;                                                                                                             \
    touch(&y, N);                                                                                                      \
    int r = live1 + live2;                                                                                             \
    for (int i = 0; i < N; i++)                                                                                        \
      r = r * 5 + y.a[i] - x.a[i];                                                                                     \
    return r;                                                                                                          \
  }
COPY_TEST(Q3, 3)
COPY_TEST(Q5, 5)
COPY_TEST(Q7, 7)
COPY_TEST(Q10, 10)

__attribute__((noinline)) static int self_assign(int k)
{
  struct Q5 x;
  struct Q5 *p = &x;
  for (int i = 0; i < 5; i++)
    x.a[i] = k + i;
  *p = x;
  return x.a[0] + x.a[4];
}

__attribute__((noinline)) static int to_volatile(int k)
{
  struct Q5 x;
  volatile struct Q5 v;
  for (int i = 0; i < 5; i++)
    x.a[i] = k - i;
  touch(&x, 5);
  for (int i = 0; i < 5; i++)
    v.a[i] = x.a[i];
  return v.a[1] + v.a[3];
}

int main(void)
{
  printf("%d %d %d %d\n", copy_Q3(2), copy_Q5(3), copy_Q7(4), copy_Q10(5));
  printf("%d %d\n", self_assign(10), to_volatile(20));
  return 0;
}
