#include <stdio.h>

int result(double a, double b) { return a + b; }

int main() {
  double a = 1.5;
  double b = 2.5;
  int res = result(a, b);
  printf("Result: %d\n", res);
  return 0;
}