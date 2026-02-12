/* Test: switch statement with direct return from each case.
 * Exercises jump table (TBH) optimization with backward targets.
 */
#include <stdio.h>

int switch_return(int x)
{
  switch (x)
  {
  case 1:
    return 10;
  case 2:
    return 20;
  case 3:
    return 30;
  case 4:
    return 40;
  case 5:
    return 50;
  case 6:
    return 60;
  case 7:
    return 70;
  default:
    return 0;
  }
}

int main(void)
{
  for (int i = 0; i <= 8; i++)
  {
    printf("switch_return(%d) = %d\n", i, switch_return(i));
  }
  return 0;
}
