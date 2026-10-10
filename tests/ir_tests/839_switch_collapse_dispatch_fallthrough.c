/* switch_collapse turned a SWITCH_TABLE whose arms all return the same value
 * into a NOP; control then fell through to whatever followed the dispatch (the
 * epilogue) instead of reaching the common target. */
#include <stdio.h>
int calls;
__attribute__((noinline)) void g(int v) { calls += v; }
__attribute__((noinline)) int k3(int x, int y)
{
  if (y)
    goto L;
  switch (x)
  {
  L:
    g(5);
  case 0: return 3;
  case 1: return 3;
  case 2: return 3;
  case 3: return 3;
  case 4: return 3;
  case 5: return 3;
  case 6: return 3;
  case 7: return 3;
  case 8: return 3;
  case 9: return 3;
  default: return 3;
  }
}
int main(void)
{
  int a = k3(4, 0);
  printf("%d %d\n", a, calls);
  int b = k3(20, 0);
  int c = k3(1, 1);
  printf("%d %d %d\n", b, c, calls);
  if (a != 3 || b != 3 || c != 3 || calls != 5)
  {
    printf("FAIL\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}
