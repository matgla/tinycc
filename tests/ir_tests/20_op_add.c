#include <stdio.h>

int simple0() { return 12312; }

int simple01() { return 0xdeadbeef; }

int simple02(int x) {
  int y = 0xdeadbeef;
  return x + y;
}

// int simple022(int x) { return 0xdeadbeef + x; }

int simple1(int x) { return 42 + x * x; }

int simple_stack(int x) {
  int a = x + 123;
  return a;
}

int simple2(int x, int y) { return x + y; }

int simple3(int x, int y, int z) { return x * y + z; }

int simple4(int x, int y, int z, int w) { return x + y + z + w; }

// // int simple5(int x, int y, int z, int w, int u, int i) {
// //   return x * y + z * w + u + i;
// // }

int main(int argc, char *argv[]) {
  int res = 0, sum = 0;
  res = simple0();
  printf("Result simple0: '%d'\n", res);
  sum += res;

  res = simple01();
  printf("Result simple01: %d\n", res);
  sum += res;

  res = simple02(1);
  printf("Result simple02: %d\n", res);
  sum += res;

  // res = simple022(10);
  // printf("Result simple022: %d\n", res);
  // sum += res;

  res = simple1(2);
  printf("Result simple1: %d\n", res);
  sum += res;

  res = simple_stack(3);
  printf("Result simple_stack: %d\n", res);
  sum += res;

  res = simple2(4, 5);
  printf("Result simple2: %d\n", res);
  sum += res;

  res = simple3(6, 7, 8);
  printf("Result simple3: %d\n", res);
  sum += res;

  res = simple4(9, 10, 11, 12);
  printf("Result simple4: %d\n", res);
  sum += res;

  // res = simple5(13, 14, 15, 16, 17, 18);
  // printf("Result simple5: %d\n", res);
  // sum += res;

  printf("Total sum: %d\n", sum);
  return 0;
}
