/* Negative postinc case: the deref sits under a condition inside the loop, so
 * it does not execute once per iteration — folding the latch bump into it
 * would advance the pointer only on taken iterations.  The transform must NOT
 * fire; the loop must still compute correctly. */
#include <stdio.h>

int arr[32];

int main(void)
{
  for (int i = 0; i < 32; i++)
    arr[i] = i * 13 + 5;

  int s = 0;
  for (int i = 0; i < 32; i++)
  {
    if (arr[i] & 1)
      s += arr[i];
  }

  printf("s=%d\n", s);
  return 0;
}
