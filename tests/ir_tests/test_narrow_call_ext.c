/* A narrow value that already fits is not extended again (ssa:narrow): a
 * byte or halfword load, a mask, a truth value, and the result of a call
 * declared to return bool or an unsigned byte or halfword -- the AAPCS has the
 * callee extend it.  A wider result stored into a narrow local must still be
 * truncated, and signed and indirect-call results keep their extension. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

__attribute__((noinline)) static uint8_t n8(uint32_t k) { return (uint8_t)(k * 7 + 3); }
__attribute__((noinline)) static uint16_t n16(uint32_t k) { return (uint16_t)(k * 40503u); }
__attribute__((noinline)) static int8_t s8(uint32_t k) { return (int8_t)(k * 37); }
__attribute__((noinline)) static bool odd(uint32_t k) { return k & 1; }
__attribute__((noinline)) static uint32_t wide(uint32_t k) { return k * 0x01010101u + 0x180; }

static uint8_t (*volatile n8p)(uint32_t) = n8;

__attribute__((noinline)) static uint32_t calls(uint32_t k)
{
  uint8_t a = n8(k);
  uint16_t b = n16(k);
  int8_t c = s8(k);
  bool d = odd(k);
  uint8_t e = (uint8_t)wide(k);  /* must truncate */
  uint16_t f = (uint16_t)wide(k + 1);
  uint8_t g = n8p(k + 2);         /* indirect */
  uint32_t r = 0;
  for (uint32_t i = 0; i < 3; i++)
    r = r * 31 + (uint32_t)a + b + (uint32_t)(int32_t)c + d + e + f + g + i;
  return r;
}

__attribute__((noinline)) static uint32_t loads(const uint8_t *p, const uint16_t *q, uint32_t m)
{
  uint8_t x = p[m & 3];
  uint16_t y = q[m & 1];
  uint16_t z = (uint16_t)(m & 0x7f);
  bool t = m > 100;
  return (uint32_t)(uint8_t)x * 3 + (uint16_t)y + (uint16_t)(uint8_t)z + (uint8_t)t;
}

int main(void)
{
  static const uint8_t p[4] = {0x81, 0xff, 7, 0x80};
  static const uint16_t q[2] = {0xfffe, 0x8001};
  printf("%u %u %u\n", calls(1), calls(200), calls(0xdeadbeef));
  printf("%u %u %u\n", loads(p, q, 0), loads(p, q, 131), loads(p, q, 0xffffffff));
  return 0;
}
