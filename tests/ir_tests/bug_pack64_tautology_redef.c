/* pack64_tautology rewrote PACK64(lo(x), hi(x)) to x read at the PACK64, after x
 * had been reassigned. */
#include <stdio.h>

__attribute__((noinline)) unsigned long long f(unsigned long long x, unsigned long long z)
{
  unsigned lo = (unsigned)x;
  unsigned hi = x >> 32;
  x = z;
  return ((unsigned long long)hi << 32) | lo;
}

/* nothing is reassigned: the repack is still the identity */
__attribute__((noinline)) unsigned long long same(unsigned long long x)
{
  unsigned lo = (unsigned)x;
  unsigned hi = x >> 32;
  return ((unsigned long long)hi << 32) | lo;
}

int main(void)
{
  unsigned long long x = 0x1122334455667788ull, z = 0xaabbccddeeff0011ull;
  printf("%llx %llx\n", f(x, z), same(x));
  return 0;
}
