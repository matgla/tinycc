#include <stdio.h>

void test(void)
{
  struct S { int x, y; } 
    c[] = {{1, 2}, {3, 4}}, 
    d[] = {{7, 8}, {9, 10}}, 
    e[] = {{11, 12}, {5, 6}};

  /* Print e alone first */
  printf("e alone: %d %d %d %d\n", e[0].x, e[0].y, e[1].x, e[1].y);
  
  /* 13 args - 3 arrays */
  printf("all: %d %d %d %d - %d %d %d %d - %d %d %d %d\n", 
         c[0].x, c[0].y, c[1].x, c[1].y, 
         d[0].x, d[0].y, d[1].x, d[1].y, 
         e[0].x, e[0].y, e[1].x, e[1].y);
}

int main(void)
{
    test();
    return 0;
}
