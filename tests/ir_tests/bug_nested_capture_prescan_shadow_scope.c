/* Nested-function capture prescan: a local declared in an inner block or
 * later in the body must not hide the parent's variable elsewhere in the body.
 * Fix: prescan only treats parameters as whole-body shadows. */
#include <stdio.h>

int x = 100;
int y = 200;

__attribute__((noinline)) int outer(int k)
{
  int x = k;
  int nested(void)
  {
    int r = x;
    {
      int x = 5;
      r += x;
    }
    return r;
  }
  return nested();
}

/* declaration later in the body; use before it must read the parent */
__attribute__((noinline)) int outer_later(int k)
{
  int y = k * 2;
  int nested(void)
  {
    int r = y;
    r += 1;
    int y = 7;
    return r + y;
  }
  return nested();
}

/* a capture that is written, with an inner shadow block afterwards */
__attribute__((noinline)) int outer_write(int k)
{
  int x = k;
  void bump(void)
  {
    x += 10;
    {
      int x = 1000;
      x++;
    }
    x += 1;
  }
  bump();
  return x;
}

/* parameter shadows the parent for the whole body */
__attribute__((noinline)) int outer_param(int k)
{
  int x = k;
  int nested(int x) { return x * 2; }
  return nested(x + 1) + x;
}

/* a pure local (no inner shadow) still shadows */
__attribute__((noinline)) int outer_local(int k)
{
  int x = k;
  int nested(void)
  {
    int x = 3;
    return x * 2;
  }
  return nested() + x;
}

int main(void)
{
  printf("%d\n", outer(1));
  printf("%d\n", outer_later(4));
  printf("%d\n", outer_write(5));
  printf("%d\n", outer_param(3));
  printf("%d\n", outer_local(10));
  return 0;
}
