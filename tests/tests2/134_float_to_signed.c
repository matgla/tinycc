#include <stdio.h>

// void print_args() {
//   printf("1 %d\n", (int)-1.0f);
//   printf("2 %d\n", (int)1.25f);
//   printf("3 %d\n", (int)-2147483648.0f);
//   printf("4 %d\n", (int)2147483648.0f);
// }

int take_float(float f) {
  return (int)f;
}

int take_int(int i) {
  return i;
}

// void print_variables() {
//   float d = -1.0;
//   float d2 = 1.25;

//   printf("5 %d\n", (int)d);
//   printf("6 %d\n", (int)d2);
//   d = -2147483648.0;
//   printf("7 %d\n", (int)d);
//   d2 = 2147483648.0;
//   printf("8 %d\n", (int)d2);
// }

int main() {
  // print_args();
  // print_variables();

  // printf("9 %llu\n", (unsigned long long)1e19f);
  int i = take_int(2);
  int f = take_float(1.5f);
  printf("10 %d\n", f);
  printf("11 %d\n", i);
}
