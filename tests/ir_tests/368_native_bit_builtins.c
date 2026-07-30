/* Guard for the native lowering of the 32-bit bit-manipulation builtins.
 *
 * __builtin_clz/ctz (and their `l` aliases) become CLZ / RBIT+CLZ, and
 * __builtin_bswap32/16 become REV / REV16, instead of calls to __clzsi2,
 * __ctzsi2 and __bswapsi2 (tccgen.c -> TCCIR_OP_CLZ/RBIT/REV/REV16).
 *
 * The cases below cover, for each builtin: a runtime value (the instruction
 * path), a value forced through memory (operand not already in a register),
 * and a compile-time constant (the fold path, which must agree with the
 * instruction path).  bswap16 in particular must produce only the swapped low
 * halfword, with the upper bits clear.
 */
#include <stdio.h>

volatile unsigned vmem;

__attribute__((noinline)) int f_clz(unsigned x) { return __builtin_clz(x); }
__attribute__((noinline)) int f_ctz(unsigned x) { return __builtin_ctz(x); }
__attribute__((noinline)) int f_clzl(unsigned long x) { return __builtin_clzl(x); }
__attribute__((noinline)) int f_ctzl(unsigned long x) { return __builtin_ctzl(x); }
__attribute__((noinline)) unsigned f_bswap32(unsigned x) { return __builtin_bswap32(x); }
__attribute__((noinline)) unsigned short f_bswap16(unsigned short x) { return __builtin_bswap16(x); }

/* clz/ctz feeding arithmetic, so the result has to survive as a value. */
__attribute__((noinline)) int f_mix(unsigned x) { return __builtin_clz(x) * 2 + __builtin_ctz(x); }

int main(void)
{
  static const unsigned vals[] = {1u, 2u, 0x80000000u, 0x00010000u, 0xFFFFFFFFu, 0x12345678u, 0x0000FF00u};
  static const int clz_exp[] = {31, 30, 0, 15, 0, 3, 16};
  static const int ctz_exp[] = {0, 1, 31, 16, 0, 3, 8};
  int n = (int)(sizeof(vals) / sizeof(vals[0]));

  for (int i = 0; i < n; i++)
  {
    unsigned v = vals[i];
    if (f_clz(v) != clz_exp[i] || f_ctz(v) != ctz_exp[i])
    {
      printf("FAIL clz/ctz %d\n", i);
      return 1;
    }
    if (f_clzl(v) != clz_exp[i] || f_ctzl(v) != ctz_exp[i])
    {
      printf("FAIL clzl/ctzl %d\n", i);
      return 2;
    }
    if (f_mix(v) != clz_exp[i] * 2 + ctz_exp[i])
    {
      printf("FAIL mix %d\n", i);
      return 3;
    }
    /* Same operand, but reached through memory. */
    vmem = v;
    if (__builtin_clz(vmem) != clz_exp[i] || __builtin_ctz(vmem) != ctz_exp[i])
    {
      printf("FAIL mem clz/ctz %d\n", i);
      return 4;
    }
  }

  /* Constant arguments must fold to the same answers. */
  if (__builtin_clz(1u) != 31 || __builtin_ctz(1u) != 0 || __builtin_clz(0x80000000u) != 0 ||
      __builtin_ctz(0x80000000u) != 31 || __builtin_clz(0x12345678u) != 3 || __builtin_ctz(0x0000FF00u) != 8)
  {
    printf("FAIL const clz/ctz\n");
    return 5;
  }

  if (f_bswap32(0x11223344u) != 0x44332211u || f_bswap32(0u) != 0u || f_bswap32(0xFF000000u) != 0x000000FFu)
  {
    printf("FAIL bswap32\n");
    return 6;
  }
  vmem = 0xAABBCCDDu;
  if (__builtin_bswap32(vmem) != 0xDDCCBBAAu || __builtin_bswap32(0x0A0B0C0Du) != 0x0D0C0B0Au)
  {
    printf("FAIL bswap32 mem/const\n");
    return 7;
  }

  if (f_bswap16(0xABCD) != 0xCDAB || f_bswap16(0x00FF) != 0xFF00 || f_bswap16(0) != 0)
  {
    printf("FAIL bswap16\n");
    return 8;
  }
  /* The upper 16 bits of the argument must not leak into the result. */
  if ((unsigned)f_bswap16((unsigned short)0x1234) != 0x3412u || __builtin_bswap16(0x0102) != 0x0201)
  {
    printf("FAIL bswap16 width\n");
    return 9;
  }

  printf("OK\n");
  return 0;
}
