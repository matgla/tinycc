/* Negative postinc case: a descending loop (step -1) has a negative stride,
 * which the post-index writeback emitter cannot encode (puw=3 is add-only).
 * The transform must NOT fire; the loop must still compute correctly. */
#include <stdio.h>

int arr[32];

int main(void)
{
  for (int i = 0; i < 32; i++)
    arr[i] = i * 11 + 1;

  int s = 0;
  for (int i = 31; i >= 0; i--)
    s += arr[i];

  printf("s=%d\n", s);
  return 0;
}
