#include <stdint.h>
#include <stdio.h>

static int check_u64(const char *name, unsigned long long got, unsigned long long exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=0x%016llX exp=0x%016llX\n", name, got, exp);
    return 1;
  }
  return 0;
}

static unsigned long long and_u64(unsigned long long a, unsigned long long b)
{
  return a & b;
}

static unsigned long long or_u64(unsigned long long a, unsigned long long b)
{
  return a | b;
}

static unsigned long long xor_u64(unsigned long long a, unsigned long long b)
{
  return a ^ b;
}

static unsigned long long not_u64(unsigned long long a)
{
  return ~a;
}

static unsigned long long shl_u64(unsigned long long a, unsigned int s)
{
  return a << s;
}

static unsigned long long shr_u64(unsigned long long a, unsigned int s)
{
  return a >> s;
}

static unsigned long long mix_ops_u64(unsigned long long a, unsigned long long b)
{
  unsigned long long t0 = (a ^ b) & 0x0F0F0F0F0F0F0F0FULL;
  unsigned long long t1 = (a | 0x8000000000000000ULL) >> 5;
  unsigned long long t2 = (b << 13) | (b >> (64 - 13));
  return (t0 ^ t1) + t2;
}

static unsigned long long byte_swap_pairs_u64(unsigned long long a)
{
  unsigned long long lo = a & 0x00FF00FF00FF00FFULL;
  unsigned long long hi = a & 0xFF00FF00FF00FF00ULL;
  return (lo << 8) | (hi >> 8);
}

static unsigned long long carry_mask_u64(unsigned long long a, unsigned long long b)
{
  unsigned long long sum = a + b;
  return (a & b) | ((a | b) & ~sum);
}

static long long shr_s64(long long a, unsigned int s)
{
  return a >> s;
}

static long long shl_s64(long long a, unsigned int s)
{
  return a << s;
}

int main(void)
{
  printf("Testing unsigned long long bitwise ops\n");

  if (check_u64("and_low", and_u64(0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL), 0x00000000FFFFFFFFULL))
    return 1;
  if (check_u64("and_high", and_u64(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL), 0xFFFFFFFF00000000ULL))
    return 1;
  if (check_u64("or_mix", or_u64(0x00000000FFFFFFFFULL, 0xFFFFFFFF00000000ULL), 0xFFFFFFFFFFFFFFFFULL))
    return 1;
  if (check_u64("xor_hi", xor_u64(0x0000000000000000ULL, 0x8000000000000000ULL), 0x8000000000000000ULL))
    return 1;
  if (check_u64("xor_lo", xor_u64(0x00000000FFFFFFFFULL, 0x00000000FFFF0000ULL), 0x000000000000FFFFULL))
    return 1;
  if (check_u64("not", not_u64(0x00FF00FF00FF00FFULL), 0xFF00FF00FF00FF00ULL))
    return 1;
  if (check_u64("shl_1", shl_u64(0x0000000080000000ULL, 1), 0x0000000100000000ULL))
    return 1;
  if (check_u64("shl_32", shl_u64(0x0000000000000001ULL, 32), 0x0000000100000000ULL))
    return 1;
  if (check_u64("shr_1", shr_u64(0x8000000000000000ULL, 1), 0x4000000000000000ULL))
    return 1;
  if (check_u64("shr_32", shr_u64(0x0000000100000000ULL, 32), 0x0000000000000001ULL))
    return 1;
  if (check_u64("shr_63", shr_u64(0x8000000000000000ULL, 63), 0x0000000000000001ULL))
    return 1;
  if (check_u64("mix_ops", mix_ops_u64(0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL), 0xC30DE09F72410DF3ULL))
    return 1;
  if (check_u64("byte_pairs", byte_swap_pairs_u64(0x1122334455667788ULL), 0x2211443366558877ULL))
    return 1;
  if (check_u64("carry_mask", carry_mask_u64(0x00000000FFFFFFFFULL, 0x0000000000000001ULL), 0x00000000FFFFFFFFULL))
    return 1;

  if (check_u64("asr_sign", (unsigned long long)shr_s64((long long)0x8000000000000000ULL, 1), 0xC000000000000000ULL))
    return 1;
  if (check_u64("asl_sign", (unsigned long long)shl_s64((long long)0x7FFFFFFFFFFFFFFFULL, 1), 0xFFFFFFFFFFFFFFFEULL))
    return 1;

  printf("PASS\n");
  return 0;
}
