#include <stdint.h>
#include <stdio.h>

static int fail_u32(const char *name, uint32_t got, uint32_t exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=0x%lx exp=0x%lx\n", name, (unsigned long)got, (unsigned long)exp);
    return 1;
  }
  return 0;
}

__attribute__((noinline)) static void mul32wide_u32(uint32_t a, uint32_t b, uint32_t *lo, uint32_t *hi)
{
  const uint32_t a0 = a & 0xFFFFu;
  const uint32_t a1 = a >> 16;
  const uint32_t b0 = b & 0xFFFFu;
  const uint32_t b1 = b >> 16;

  const uint32_t p0 = a0 * b0;
  const uint32_t p1 = a0 * b1;
  const uint32_t p2 = a1 * b0;
  const uint32_t p3 = a1 * b1;

  const uint32_t mid = (p0 >> 16) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);
  *lo = (p0 & 0xFFFFu) | (mid << 16);
  *hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
}

static int check_pair(uint32_t a, uint32_t b)
{
  volatile uint32_t lo = 0xDEADBEEFu;
  volatile uint32_t hi = 0xCAFEBABEu;

  mul32wide_u32(a, b, (uint32_t *)&lo, (uint32_t *)&hi);

  const uint64_t p = (uint64_t)a * (uint64_t)b;
  const uint32_t exp_lo = (uint32_t)p;
  const uint32_t exp_hi = (uint32_t)(p >> 32);

  int fails = 0;
  fails |= fail_u32("lo", lo, exp_lo);
  fails |= fail_u32("hi", hi, exp_hi);
  return fails;
}

int main(void)
{
  int fails = 0;

  fails |= check_pair(0x00000000u, 0x00000000u);
  fails |= check_pair(0x00000001u, 0x00000001u);
  fails |= check_pair(0x00010001u, 0x00010001u);
  fails |= check_pair(0xFFFF0001u, 0x0002FFFFu);
  fails |= check_pair(0xFFFFFFFFu, 0xFFFFFFFFu);
  fails |= check_pair(0x80000000u, 0x80000000u);
  fails |= check_pair(0x12345678u, 0x9ABCDEF0u);

  if (fails)
    return 1;
  printf("PASS\n");
  return 0;
}
