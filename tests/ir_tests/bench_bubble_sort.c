#include <stdio.h>

int bench_bubble_sort(void)
{
  int arr[64];
  int checksum = 0;

  for (int i = 0; i < 64; i++) {
      arr[i] = (63 - i) * 7 + 100;
  }

  for (int i = 0; i < 63; i++) {
      for (int j = 0; j < 63 - i; j++) {
          if (arr[j] > arr[j + 1]) {
              int temp = arr[j];
              arr[j] = arr[j + 1];
              arr[j + 1] = temp;
          }
      }
  }

  checksum = 0;
  for (int i = 0; i < 64; i++) {
      checksum += arr[i] * i;
  }

  return checksum;
}

int main(void)
{
    printf("bubble_sort: %d\n", bench_bubble_sort());
    return 0;
}
