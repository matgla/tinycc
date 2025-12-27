// #include <stdio.h>
extern int printf(const char *, ...);

int main() {
  int a;
  int b;
  int c;
  int d;
  int e;
  int f;
  int x;
  int y;

  a = 12;
  b = 34;
  c = 56;
  d = 78;
  e = 0;
  f = 1;

  printf("%d\n", c + d);
  printf("%d\n", (y = c + d));
  printf("%d\n", e || e && f);
  printf("%d\n", e || f && f);
  printf("%d\n", e && e || f);
  printf("%d\n", e && f || f);
  printf("%d\n", a && f | f);
  printf("%d\n", a | b ^ c & d);
  printf("%d, %d\n", a == a, a == b);
  printf("%d, %d\n", a != a, a != b);
  printf("%d\n", a != b && c != d);
  printf("%d\n", a + b * c / f);
  printf("%d\n", a + b * c / f);
  printf("%d\n", (4 << 4));
  printf("%d\n", (64 >> 4));

  return 0;
}

// // vim: set expandtab ts=4 sw=3 sts=3 tw=80 :

// #include <stdio.h>

// int main(int argc, char *argv[]) {
//   int x = 123;

//   if (x > 100) {
//     x = x - 100;
//     printf("x is greater than 100\n");
//   }

//   if (x == 23) {
//     printf("x is 23 \n");
//   }

//   if (x == 22) {
//     printf("x is 22 \n");
//   } else {
//     printf("x is not 22 \n");
//   }

//   return 0;
// }