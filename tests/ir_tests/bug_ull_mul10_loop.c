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
  unsigned i;
  unsigned long long v = 1;
  for (i = 0; i < 14; i++)
  {
    U64 u;
    u.ull = v;
    printf("%u hi=%08x lo=%08x\n", i, u.s.hi, u.s.lo);
    v *= 10ULL;
  }
  return 0;
}
