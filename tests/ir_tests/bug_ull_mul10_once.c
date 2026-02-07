#include <stdint.h>
#include <stdio.h>

typedef union
{
  unsigned long long ull;
  struct
  {
    unsigned lo;
    unsigned hi;
  } s;
} U64;

int main(void)
{
  U64 u;
  u.ull = 1000000000ULL;
  u.ull *= 10ULL;
  printf("hi=%08x lo=%08x\n", u.s.hi, u.s.lo);
  return 0;
}
