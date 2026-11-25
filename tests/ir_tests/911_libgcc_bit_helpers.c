/* libgcc's bit-counting entry points in libtcc1 (lib/builtin.c): __clzsi2,
 * __ctzsi2, __popcountsi2, __paritysi2 and the di2 forms.  gcc and clang call
 * them for __builtin_clz and friends; tcc links them for objects those
 * compilers built (zig's C among them).  They used to be the YasOS libc's
 * (bitops.c), and only __ctzsi2/__popcountsi2 were here, as weak fallbacks.
 *
 * Called by name, so the compiler's own lowering of the builtins
 * (368_native_bit_builtins) is not what is tested.  clz/ctz of 0 are the
 * width, the answer the libc gave and the CLZ instruction gives. */
#include <stdio.h>

int __clzsi2(unsigned int x);
int __ctzsi2(unsigned int x);
int __popcountsi2(unsigned int x);
int __paritysi2(unsigned int x);
int __clzdi2(unsigned long long x);
int __ctzdi2(unsigned long long x);
int __popcountdi2(unsigned long long x);
int __paritydi2(unsigned long long x);

/* through memory, so no call is folded */
volatile unsigned int v32[] = {0u, 1u, 2u, 0x80000000u, 0x00010000u, 0xFFFFFFFFu, 0x12345678u, 0x0000FF00u};
static const int clz32[] = {32, 31, 30, 0, 15, 0, 3, 16};
static const int ctz32[] = {32, 0, 1, 31, 16, 0, 3, 8};
static const int pop32[] = {0, 1, 1, 1, 1, 32, 13, 8};

volatile unsigned long long v64[] = {0ull, 1ull, 0x8000000000000000ull, 0x0000000100000000ull,
                                     0xFFFFFFFFFFFFFFFFull, 0x123456789ABCDEF0ull, 0x00000000FFFFFFFFull};
static const int clz64[] = {64, 63, 0, 31, 0, 3, 32};
static const int ctz64[] = {64, 0, 63, 32, 0, 4, 0};
static const int pop64[] = {0, 1, 1, 1, 64, 32, 32};

static int ref_clz(unsigned long long v, int w)
{
  for (int i = w - 1; i >= 0; i--)
    if (v & (1ull << i))
      return w - 1 - i;
  return w;
}

static int ref_ctz(unsigned long long v, int w)
{
  for (int i = 0; i < w; i++)
    if (v & (1ull << i))
      return i;
  return w;
}

static int ref_popcount(unsigned long long v)
{
  int n = 0;
  for (int i = 0; i < 64; i++)
    n += (int)((v >> i) & 1ull);
  return n;
}

int main(void)
{
  int n32 = (int)(sizeof clz32 / sizeof clz32[0]);
  int n64 = (int)(sizeof clz64 / sizeof clz64[0]);

  for (int i = 0; i < n32; i++)
  {
    unsigned int x = v32[i];
    if (__clzsi2(x) != clz32[i] || __ctzsi2(x) != ctz32[i])
    {
      printf("FAIL si2 clz/ctz %d: %d %d\n", i, __clzsi2(x), __ctzsi2(x));
      return 1;
    }
    if (__popcountsi2(x) != pop32[i] || __paritysi2(x) != (pop32[i] & 1))
    {
      printf("FAIL si2 popcount/parity %d: %d %d\n", i, __popcountsi2(x), __paritysi2(x));
      return 2;
    }
  }

  for (int i = 0; i < n64; i++)
  {
    unsigned long long x = v64[i];
    if (__clzdi2(x) != clz64[i] || __ctzdi2(x) != ctz64[i])
    {
      printf("FAIL di2 clz/ctz %d: %d %d\n", i, __clzdi2(x), __ctzdi2(x));
      return 3;
    }
    if (__popcountdi2(x) != pop64[i] || __paritydi2(x) != (pop64[i] & 1))
    {
      printf("FAIL di2 popcount/parity %d: %d %d\n", i, __popcountdi2(x), __paritydi2(x));
      return 4;
    }
  }

  /* every bit position, alone, below a lower bit, and with every higher bit
     set: the high word of the di2 forms included */
  for (int b = 0; b < 64; b++)
  {
    unsigned long long bit = 1ull << b, from = ~0ull << b;
    if (__clzdi2(bit) != 63 - b || __clzdi2(bit | (bit >> 1) | 1ull) != 63 - b || __ctzdi2(bit) != b ||
        __ctzdi2(from) != b || __popcountdi2(bit - 1ull) != b || __popcountdi2(from) != 64 - b ||
        __paritydi2(bit - 1ull) != (b & 1) || __paritydi2(bit) != 1)
    {
      printf("FAIL di2 bit %d\n", b);
      return 5;
    }
    if (b < 32)
    {
      unsigned int bit32 = 1u << b, from32 = ~0u << b;
      if (__clzsi2(bit32) != 31 - b || __clzsi2(bit32 | (bit32 >> 1) | 1u) != 31 - b || __ctzsi2(bit32) != b ||
          __ctzsi2(from32) != b || __popcountsi2(bit32 - 1u) != b || __popcountsi2(from32) != 32 - b ||
          __paritysi2(bit32 - 1u) != (b & 1))
      {
        printf("FAIL si2 bit %d\n", b);
        return 6;
      }
    }
  }

  /* pseudo-random values against bit-by-bit references (xorshift64, thinned
     out so that long zero runs occur) */
  unsigned long long x = 0x9E3779B97F4A7C15ull;
  for (int n = 0; n < 2000; n++)
  {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    unsigned long long v = x;
    if (n & 1)
      v &= x << (n % 61);
    if (n & 2)
      v >>= n % 63;
    unsigned int v32 = (unsigned int)v;
    if (__clzsi2(v32) != ref_clz(v32, 32) || __clzdi2(v) != ref_clz(v, 64) || __ctzsi2(v32) != ref_ctz(v32, 32) ||
        __ctzdi2(v) != ref_ctz(v, 64) || __popcountsi2(v32) != ref_popcount(v32) ||
        __popcountdi2(v) != ref_popcount(v) || __paritysi2(v32) != (ref_popcount(v32) & 1) ||
        __paritydi2(v) != (ref_popcount(v) & 1))
    {
      printf("FAIL random %d: %llx\n", n, v);
      return 7;
    }
  }

  printf("OK\n");
  return 0;
}
