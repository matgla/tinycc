/* Regression test: constant-folded bswap must convert its signed argument
 * to the builtin's unsigned parameter width before swapping. */
#include <stdio.h>

volatile short vs = -2;
volatile int vi = -2;
volatile long long vll = -2;

int main(void)
{
  unsigned c32_int = __builtin_bswap32(-2);
  unsigned r32_int = __builtin_bswap32(vi);
  unsigned c32_short = __builtin_bswap32((short)-2);
  unsigned r32_short = __builtin_bswap32(vs);
  unsigned long long c64_int = __builtin_bswap64(-2);
  unsigned long long r64_int = __builtin_bswap64(vi);
  unsigned long long c64_short = __builtin_bswap64((short)-2);
  unsigned long long r64_short = __builtin_bswap64(vs);
  unsigned long long c64_ll = __builtin_bswap64(-2LL);
  unsigned long long r64_ll = __builtin_bswap64(vll);

  printf("%08x %08x %08x %08x\n", c32_int, r32_int, c32_short, r32_short);
  printf("%016llx %016llx %016llx %016llx %016llx %016llx\n",
         c64_int, r64_int, c64_short, r64_short, c64_ll, r64_ll);
  return 0;
}
