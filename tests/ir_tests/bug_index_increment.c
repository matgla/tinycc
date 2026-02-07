#include <stdio.h>

int main()
{
  int index = 10;

  printf("Before: index = %d\n", index);

  // This increment should update index
  index += 1;

  printf("After: index = %d\n", index);

  return 0;
}
