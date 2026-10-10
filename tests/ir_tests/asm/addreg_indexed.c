/* arm_addreg_indexed: an address `base + index` of two registers feeding only
 * loads and stores becomes the [Rn, Rm] operand -- Zig's mem.findSentinel
 * scans a C string through `&p[i]`. */
#include <stdint.h>

uintptr_t find_sentinel(const uint8_t *const a0)
{
  const uint8_t *const *t1;
  uintptr_t t2, t3;
  const uint8_t *t0, *t4, *t5;
  t0 = a0;
  t1 = (const uint8_t *const *)&t0;
  t2 = 0;
loop:
  t3 = t2;
  t4 = (*t1);
  t5 = &t4[t3];
  if (*t5 != 0)
  {
    t2 = t3 + 1;
    goto loop;
  }
  return t2;
}

void copy_halves(uint16_t *dst, const int16_t *src, uintptr_t n)
{
  for (uintptr_t i = 0; i < n; i++)
    *(uint16_t *)((uintptr_t)dst + (i << 1)) = (uint16_t)(*(const int16_t *)((uintptr_t)src + (i << 1)) + 1);
}

/* LDRD has no [Rn, Rm] form: left alone. */
uint64_t get64(const uint8_t *p, uintptr_t off)
{
  return *(const uint64_t *)(p + off);
}
