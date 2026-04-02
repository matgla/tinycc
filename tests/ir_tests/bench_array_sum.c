#include <stdio.h>

int bench_array_sum(void)
{
  int arr[256];
  int sum = 0;
  int iterations = 100;

  for (int i = 0; i < 256; i++) {
    arr[i] = i * 7 + 13;
  }

  for (int n = 0; n < iterations; n++) {
    sum = 0;
    for (int i = 0; i < 256; i++) {
      sum += arr[i];
    }
  }

  return sum;
}

int main(void)
{
    printf("array_sum: %d\n", bench_array_sum());
    return 0;
}
