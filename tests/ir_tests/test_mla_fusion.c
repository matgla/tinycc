#include <stdio.h>

/* Test MLA (Multiply-Accumulate) fusion optimization
 * The compiler should fuse: temp = a * b; result = temp + c;
 * Into: result = MLA(a, b, c)
 */

/* Simple MLA pattern: return a * b + c */
int mla_simple(int a, int b, int c)
{
  return a * b + c;
}

/* MLA pattern with swapped operands: return c + a * b */
int mla_swapped(int a, int b, int c)
{
  return c + a * b;
}

/* Multiple MLA patterns */
int mla_multiple(int a, int b, int c, int d)
{
  int x = a * b + c;  /* MLA 1 */
  int y = x * d + 5;  /* MLA 2 */
  return y;
}

/* MLA in a loop */
int mla_loop(int n, int a, int b)
{
  int sum = 0;
  for (int i = 0; i < n; i++)
  {
    sum = sum + i * a;  /* Should fuse: sum = MLA(i, a, sum) */
  }
  return sum;
}

/* Complex expression with MLA */
int mla_complex(int a, int b, int c, int d)
{
  return a * b + c * d + a * c;  /* Should have multiple MLA opportunities */
}

int main(int argc, char *argv[])
{
  int res = 0;
  int sum = 0;

  (void)argc;
  (void)argv;

  /* Test simple MLA */
  res = mla_simple(3, 4, 5);
  printf("mla_simple(3, 4, 5) = %d (expected 17)\n", res);
  if (res != 17)
  {
    printf("FAIL: mla_simple\n");
    return 1;
  }
  sum += res;

  /* Test swapped MLA */
  res = mla_swapped(3, 4, 5);
  printf("mla_swapped(3, 4, 5) = %d (expected 17)\n", res);
  if (res != 17)
  {
    printf("FAIL: mla_swapped\n");
    return 1;
  }
  sum += res;

  /* Test multiple MLA */
  res = mla_multiple(2, 3, 4, 5);
  printf("mla_multiple(2, 3, 4, 5) = %d (expected 55)\n", res);
  if (res != 55)
  {
    printf("FAIL: mla_multiple\n");
    return 1;
  }
  sum += res;

  /* Test MLA in loop */
  res = mla_loop(5, 2, 0);
  printf("mla_loop(5, 2, 0) = %d (expected 20)\n", res);
  if (res != 20)
  {
    printf("FAIL: mla_loop\n");
    return 1;
  }
  sum += res;

  /* Test complex MLA */
  res = mla_complex(2, 3, 4, 5);
  printf("mla_complex(2, 3, 4, 5) = %d (expected 34)\n", res);
  if (res != 34)
  {
    printf("FAIL: mla_complex\n");
    return 1;
  }
  sum += res;

  printf("All MLA tests passed! Sum: %d\n", sum);
  return 0;
}
