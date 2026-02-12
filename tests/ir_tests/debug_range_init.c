#include <stdio.h>

int main(void)
{
  int c = 0;
  int dd[] = {[0 ... 1] = ++c, [2 ... 3] = ++c};
  int i;
  for (i = 0; i < 4; i++)
    printf(" %d", dd[i]);
  printf("\n");
  return 0;
}
