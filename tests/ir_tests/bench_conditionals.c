#include <stdio.h>

int bench_conditionals(void)
{
  int r = 0;
  int iterations = 1000;

  for (int n = 0; n < iterations; n++) {
    int i = 42;

    r = 1234;
    if (i & 1) {
      r += i * 3;
    } else if (i % 3 == 0) {
      r -= i;
    } else {
      r ^= i;
    }

    if (r > 1000000) {
      r = r >> 3;
    } else if (r < -1000000) {
      r = -r;
    }
  }

  return r;
}

int main(void)
{
    printf("conditionals: %d\n", bench_conditionals());
    return 0;
}
