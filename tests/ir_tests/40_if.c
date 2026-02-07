#include <stdio.h>

int main(int argc, char *argv[]) {
  int x = 123;

  if (x > 100) {
    x = x - 100;
    printf("x is greater than 100\n");
  }

  if (x == 23) {
    printf("x is 23 \n");
  }

  if (x == 22) {
    printf("x is 22 \n");
  } else {
    printf("x is not 22 \n");
  }

  return 0;
}