#include <stdio.h>

static int sum40(int a01, int a02, int a03, int a04, int a05, int a06, int a07, int a08, int a09, int a10, int a11,
                 int a12, int a13, int a14, int a15, int a16, int a17, int a18, int a19, int a20, int a21, int a22,
                 int a23, int a24, int a25, int a26, int a27, int a28, int a29, int a30, int a31, int a32, int a33,
                 int a34, int a35, int a36, int a37, int a38, int a39, int a40)
{
  return a01 + a02 + a03 + a04 + a05 + a06 + a07 + a08 + a09 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 +
         a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27 + a28 + a29 + a30 + a31 + a32 + a33 + a34 + a35 + a36 +
         a37 + a38 + a39 + a40;
}

int main(void)
{
  int s = sum40(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
                29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40);
  printf("sum=%d\n", s);
  return 0;
}
