/* Negative postinc case: sub-word (char) element accesses are excluded from
 * postinc fusion by the INT32 gate in IV analysis.  The transform must NOT
 * fire; the loop must still compute correctly. */
#include <stdio.h>

char arr[48];

int main(void)
{
  for (int i = 0; i < 48; i++)
    arr[i] = (char)(i * 7 + 1);

  int s = 0;
  for (int i = 0; i < 48; i++)
    s += (unsigned char)arr[i];

  printf("s=%d\n", s);
  return 0;
}
