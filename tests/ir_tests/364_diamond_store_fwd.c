#include <stdio.h>

int a[8];
signed char b[8];

static int same_const(int c, int i)
{
  if (c)
    a[i] = 5;
  else
    a[i] = 5;
  return a[i];
}

static int diff_const(int c, int i)
{
  if (c)
    a[i] = 7;
  else
    a[i] = 5;
  return a[i];
}

static int goto_into_merge(int c, int i)
{
  if (c) {
    a[i] = 9;
    goto out;
  }
  if (i & 1)
    a[i] = 5;
  else
    a[i] = 5;
out:
  return a[i];
}

static int byte_trunc(int c, int i)
{
  if (c)
    b[i] = 511;
  else
    b[i] = 767;
  return b[i];
}

int (*volatile fp)(int, int);

int main(void)
{
  fp = same_const;
  printf("%d %d\n", fp(1, 0), fp(0, 1));
  fp = diff_const;
  printf("%d %d\n", fp(1, 2), fp(0, 3));
  fp = goto_into_merge;
  printf("%d %d\n", fp(1, 4), fp(0, 5));
  fp = byte_trunc;
  printf("%d %d\n", fp(1, 6), fp(0, 7));
  return 0;
}
