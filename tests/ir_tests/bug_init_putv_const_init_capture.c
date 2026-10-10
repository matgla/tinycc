/*
 * Local vector initializer must capture constants after converting them to the
 * element type (init_putv const_init_data), so folded vector ops see the
 * stored value rather than the raw double/float bits.
 */
#include <stdio.h>

typedef int v4 __attribute__((vector_size(16)));
typedef float f4 __attribute__((vector_size(16)));
typedef short s4 __attribute__((vector_size(8)));

__attribute__((noinline)) int f1(void)
{
  v4 a = {1.9, 2.5, 3.0, 4.0};
  v4 c = a + a;
  return c[0] * 1000 + c[1] * 100 + c[2] * 10 + c[3];
}

__attribute__((noinline)) int f2(void)
{
  f4 a = {1, 2.5, 3, 4};
  f4 c = a + a;
  return (int)(c[0] * 10) + (int)(c[1] * 10) * 100;
}

__attribute__((noinline)) int f3(void)
{
  v4 a = {1, 2, 3, 4};
  v4 c = a + a;
  return c[0] + c[1] + c[2] + c[3];
}

__attribute__((noinline)) int f4_(void)
{
  s4 a = {-1.9, 2.5, 3, 4};
  s4 c = a + a;
  return c[0] * 1000 + c[1] * 100 + c[2] * 10 + c[3];
}

int main(void)
{
  printf("f1=%d\n", f1());
  printf("f2=%d\n", f2());
  printf("f3=%d\n", f3());
  printf("f4=%d\n", f4_());
  return 0;
}
