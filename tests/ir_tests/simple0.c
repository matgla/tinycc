#include <stdio.h>

int sum(int a, int b) { return a + b; }

int main(int argc, char *argv[]) {
  printf("Hello world\n");
  int x = sum(1, 2);
  printf("Sum: %d\n", x);
  return 0;
}