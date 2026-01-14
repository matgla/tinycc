#include <stdint.h>
#include <stdio.h>

static int ibdg_small(long long n)
{
  switch (n)
  {
  case 1LL ... 9LL:
    return 1;
  case 10LL ... 99LL:
    return 2;
  case 100LL ... 999LL:
    return 3;
  case -99LL ... - 1LL:
    return 9;
  default:
    return 0;
  }
}

static int ubdg_small(unsigned long long n)
{
  switch (n)
  {
  case 1ULL ... 9ULL:
    return 1;
  case 10ULL ... 99ULL:
    return 2;
  case 100ULL ... 999ULL:
    return 3;
  case 1000ULL ... 9999ULL:
    return 4;
  default:
    return 0;
  }
}

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

  /* Signed path */
  {
    long long v = 1;
    for (i = 0; i < 14; i++)
    {
      U64 u;
      u.ull = (unsigned long long)v;
      printf("S %u hi=%08x lo=%08x cls=%d\n", i, u.s.hi, u.s.lo, ibdg_small(v));
      v *= 10;
    }
  }

  /* Unsigned path */
  {
    unsigned long long v = 1;
    for (i = 0; i < 14; i++)
    {
      U64 u;
      u.ull = v;
      printf("U %u hi=%08x lo=%08x cls=%d\n", i, u.s.hi, u.s.lo, ubdg_small(v));
      v *= 10;
    }
  }

  return 0;
}
