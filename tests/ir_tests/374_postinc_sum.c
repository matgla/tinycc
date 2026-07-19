/* Post-index writeback addressing (opt_postinc_fusion, -O2): a straight-line
 * int array sum loop must lower to `ldr.w rV, [rP], #4` with the IV counter
 * eliminated (exit cmp on the pointer vs a hoisted end pointer). */
#include <stdio.h>

int arr[32];

int main(void)
{
  for (int i = 0; i < 32; i++)
    arr[i] = i * 3 + 1;

  int s = 0;
  for (int i = 0; i < 32; i++)
    s += arr[i];

  printf("s=%d\n", s);
  return 0;
}
