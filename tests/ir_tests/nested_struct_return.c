/* nested_struct_return.c — Phase 2: Nested function returns struct by value */
#include <stdio.h>

typedef struct
{
  int x;
  int y;
} Point;

int main(void)
{
  int dx = 10, dy = 20;

  Point offset(Point p)
  {
    Point r;
    r.x = p.x + dx;
    r.y = p.y + dy;
    return r;
  }

  Point p = {1, 2};
  Point q = offset(p);
  printf("%d %d\n", q.x, q.y);

  dx = 100;
  dy = 200;
  q = offset(p);
  printf("%d %d\n", q.x, q.y);
  return 0;
}
