/* nested_direct_call_args.c — Phase 2: Arguments + captured vars combined */
#include <stdio.h>

int main(void)
{
  int offset = 100;

  int apply(int x, int y)
  {
    return offset + x * y;
  }

  printf("%d\n", apply(3, 4));
  offset = 0;
  printf("%d\n", apply(3, 4));
  printf("%d\n", apply(7, 6));
  return 0;
}
