/* arm_addreg_indexed: loads and stores through `base + index` of two
 * registers use [Rn, Rm].  Scans, byte/halfword/word and signed accesses,
 * stores, and an index redefined between the address and the access. */
#include <stdint.h>

__attribute__((noinline)) static uintptr_t scan(const uint8_t *p)
{
  uintptr_t i = 0;
  while (*(const uint8_t *)((uintptr_t)p + i) != 0)
    i++;
  return i;
}

__attribute__((noinline)) static int32_t sum_signed(const int8_t *p, uintptr_t n)
{
  int32_t s = 0;
  for (uintptr_t i = 0; i < n; i++)
    s += *(const int8_t *)((uintptr_t)p + i) * (int32_t)(i + 1);
  return s;
}

__attribute__((noinline)) static void bump(uint8_t *dst, const uint8_t *src, uintptr_t n)
{
  for (uintptr_t i = 0; i < n; i++)
    *(uint8_t *)((uintptr_t)dst + i) = (uint8_t)(*(const uint8_t *)((uintptr_t)src + i) * 3u + 1u);
}

__attribute__((noinline)) static uint32_t words(const uint8_t *base, uintptr_t off1, uintptr_t off2)
{
  uint32_t a = *(const uint32_t *)((uintptr_t)base + off1);
  uint16_t b = *(const uint16_t *)((uintptr_t)base + off2);
  return a ^ ((uint32_t)b << 7);
}

/* the index changes after the address is formed */
__attribute__((noinline)) static uint32_t redefined(const uint8_t *p, uintptr_t i)
{
  const uint8_t *q = (const uint8_t *)((uintptr_t)p + i);
  i = i * 2u + 1u;
  return (uint32_t)*q * 1000u + (uint32_t)*(const uint8_t *)((uintptr_t)p + i);
}

static uint8_t buf[64] __attribute__((aligned(4)));
static int8_t sbuf[16] = {-1, 2, -3, 4, -5, 6, -7, 8, -9, 10, -11, 12, -13, 14, -15, 16};

int main(void)
{
  for (int i = 0; i < 64; i++)
    buf[i] = (uint8_t)(i * 37 + 11);
  buf[23] = 0;
  if (scan(buf) != 23 || scan(buf + 23) != 0 || scan(buf + 5) != 18)
    return 1;
  if (sum_signed(sbuf, 16) != 136)
    return 2;
  uint8_t out[16];
  bump(out, buf, 16);
  for (int i = 0; i < 16; i++)
    if (out[i] != (uint8_t)(buf[i] * 3u + 1u))
      return 3;
  if (words(buf, 8, 14) != (((uint32_t)buf[8] | (uint32_t)buf[9] << 8 | (uint32_t)buf[10] << 16 | (uint32_t)buf[11] << 24) ^
                            ((uint32_t)(buf[14] | buf[15] << 8) << 7)))
    return 4;
  if (redefined(buf, 3) != (uint32_t)buf[3] * 1000u + buf[7])
    return 5;
  return 0;
}
