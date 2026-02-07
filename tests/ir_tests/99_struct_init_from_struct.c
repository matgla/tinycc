#include <stdio.h>

struct S
{
  int x, y;
};

int main(void)
{
  struct S a = {1, 2};
  struct S b = {3, 4};
  struct S c[2] = {a, b};
  struct S e[2] = {b, (struct S){5, 6}};

  printf("a: %d %d\n", a.x, a.y);
  printf("b: %d %d\n", b.x, b.y);
  printf("c[0]: %d %d\n", c[0].x, c[0].y);
  printf("c[1]: %d %d\n", c[1].x, c[1].y);
  printf("e[0]: %d %d\n", e[0].x, e[0].y);
  printf("e[1]: %d %d\n", e[1].x, e[1].y);
  return 0;
}
