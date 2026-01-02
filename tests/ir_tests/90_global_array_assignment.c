#include <stdio.h>
int array[16];
int main()
{
  int i;
  array[0] = 62;
  array[1] = 37;
  for (i = 0; i < 2; i++)
    printf("%d: %d\n", i, array[i]);
  return 0;
}
