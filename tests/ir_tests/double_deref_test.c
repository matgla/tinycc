#include <stdio.h>

int A[4] = {1, 2, 3, 4};
int B[4] = {0, 0, 0, 0};

int Move(int *source, int *dest)
{
  int i = 0, j = 0;

  while (j < 4 && dest[j] == 0)
    j++;

  dest[j - 1] = source[i];
  return dest[j - 1];
}

int main()
{
  int r = Move(A, B);
  printf("result: %d\n", r);
  return r;
}