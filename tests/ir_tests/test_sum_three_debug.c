#include <stdio.h>

typedef struct {
  int a;
  int b;
} Pair;

static int sum_three(Pair p, int x, Pair q, int y) {
  printf("p.a=%d p.b=%d x=%d q.a=%d q.b=%d y=%d\n", p.a, p.b, x, q.a, q.b, y);
  return p.a + x + q.b + y;
}

int main(void) {
  int result = sum_three((Pair){5, 6}, 7, (Pair){8, 9}, 10);
  printf("result=%d expected=31\n", result);
  return 0;
}
