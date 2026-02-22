/* nested_multi_level.c — Phase 2+: Double-nested: f → g → h with chain-of-chains */
#include <stdio.h>

int main(void)
{
  int a = 1;

  int level1(int x)
  {
    int b = 20;

    int level2(int y)
    {
      /* Access grandparent 'a' via chain-of-chains
         and parent 'b' via direct chain */
      return a + b + x + y;
    }

    return level2(300);
  }

  printf("%d\n", level1(10));
  a = 100;
  printf("%d\n", level1(10));
  return 0;
}
