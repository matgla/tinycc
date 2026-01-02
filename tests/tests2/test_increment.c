#include <stdio.h>

int main()
{
  int index = 5;

  // This should compile to: load, add, store
  index += 1;

  printf("index = %d\n", index);

  return 0;
}
