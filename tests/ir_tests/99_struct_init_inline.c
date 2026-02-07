/* Test with inline struct definition like the original 90_struct-init.c */
#include <stdio.h>

void test_inline_struct(void)
{
  int i = 0;
  struct S
  {
    int x, y;
  } a = {1, 2}, b = {3, 4}, c[] = {a, b}, d[] = {++i, ++i, ++i, ++i}, e[] = {b, (struct S){5, 6}};

  printf("c[0]: %d %d, c[1]: %d %d\n", c[0].x, c[0].y, c[1].x, c[1].y);
  printf("d[0]: %d %d, d[1]: %d %d\n", d[0].x, d[0].y, d[1].x, d[1].y);
  printf("e[0]: %d %d, e[1]: %d %d\n", e[0].x, e[0].y, e[1].x, e[1].y);
}

int main(void)
{
  test_inline_struct();
  return 0;
}
