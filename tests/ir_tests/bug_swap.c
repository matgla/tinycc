#include <stdio.h>

int array[4];

void swap(int a, int b)
{
  int tmp = array[a];
  array[a] = array[b];
  array[b] = tmp;
}

int main()
{
  array[0] = 10;
  array[1] = 20;
  array[2] = 30;
  array[3] = 40;

  printf("Before swap: %d %d %d %d\n", array[0], array[1], array[2], array[3]);

  swap(0, 1);
  printf("After swap(0,1): %d %d %d %d\n", array[0], array[1], array[2], array[3]);

  swap(2, 3);
  printf("After swap(2,3): %d %d %d %d\n", array[0], array[1], array[2], array[3]);

  return 0;
}
