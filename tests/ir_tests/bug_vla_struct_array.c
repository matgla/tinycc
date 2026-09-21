/* An array of structs with a VLA member (GNU C) is itself variable-length.
 *
 * The array's type was fixed-size, so it got a frame slot sized as if the VLA
 * member were empty, and writing its elements ran over the objects next to it
 * (gcc.c-torture/execute/align-nest.c overwrote the caller's saved LR once the
 * frame layout shifted by a word). */
#include <stdio.h>

__attribute__((noinline)) static int fill(int n)
{
  volatile int above[4] = {11, 12, 13, 14};
  struct S
  {
    int i[n];
    int tail;
  } s[3];
  volatile int below[4] = {21, 22, 23, 24};

  for (int k = 0; k < 3; k++)
  {
    for (int j = 0; j < n; j++)
      s[k].i[j] = 100 * k + j;
    s[k].tail = -k;
  }
  int sum = 0;
  for (int k = 0; k < 3; k++)
  {
    for (int j = 0; j < n; j++)
      sum += s[k].i[j];
    sum += s[k].tail;
  }
  for (int j = 0; j < 4; j++)
    sum += above[j] * 1000 + below[j] * 100000;
  return sum + (int)sizeof s;
}

int main(void)
{
  printf("%d\n", fill(5));
  printf("%d\n", fill(1));
  return 0;
}
