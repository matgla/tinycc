#include <stdio.h>

__attribute__((noinline)) unsigned long long u1(unsigned x, unsigned y, int c)
{
  unsigned long long a = 0x100000000ULL;
  if (c)
    a = x;
  return a * (unsigned long long)y;
}

__attribute__((noinline)) long long m1(int x, int y, int c)
{
  long long a = 0x100000000LL;
  if (c)
    a = (long long)x;
  return a * (long long)y;
}

int main(void)
{
  unsigned long long u = u1(3, 2, 0);
  long long m = m1(3, 2, 0);

  printf("u1=%llu m1=%lld\n", u, m);
  printf("expected u1=8589934592 m1=8589934592\n");
  if (u == 8589934592ULL && m == 8589934592LL)
  {
    printf("PASS\n");
    return 0;
  }
  printf("FAIL\n");
  return 1;
}
