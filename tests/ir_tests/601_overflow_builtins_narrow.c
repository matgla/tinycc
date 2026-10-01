/* __builtin_{add,sub,mul}_overflow on 8/16/32-bit results against 64-bit
 * reference arithmetic.  32-bit add/sub with operands of the result's width
 * and signedness take the r < a / sign-of-xor checks at 32 bits; 8/16-bit
 * results are computed in a 32-bit int instead of long long (unsigned for
 * u16 * u16); mixed-type calls keep the general path. */
#include <stdint.h>
#include <stdio.h>

static const int64_t vals[] = {0, 1, 2, 3, 7, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000,
                               0x7fffffff, 0x80000000LL, 0xffffffffLL, -1, -2, -0x80, -0x81, -0x8000,
                               -0x8001, -0x7fffffffLL, -0x80000000LL, 12345, -12345, 0x1234567, 250};
#define NV (int)(sizeof vals / sizeof vals[0])

static uint32_t h = 2166136261u;
static void mix(uint32_t x) { h = (h ^ x) * 16777619u; }
static unsigned bad;

__attribute__((noinline)) static int add_u8(uint8_t a, uint8_t b, uint8_t *r) { return __builtin_add_overflow(a, b, r); }
__attribute__((noinline)) static int sub_u8(uint8_t a, uint8_t b, uint8_t *r) { return __builtin_sub_overflow(a, b, r); }
__attribute__((noinline)) static int mul_u8(uint8_t a, uint8_t b, uint8_t *r) { return __builtin_mul_overflow(a, b, r); }
__attribute__((noinline)) static int add_i8(int8_t a, int8_t b, int8_t *r) { return __builtin_add_overflow(a, b, r); }
__attribute__((noinline)) static int sub_i8(int8_t a, int8_t b, int8_t *r) { return __builtin_sub_overflow(a, b, r); }
__attribute__((noinline)) static int mul_i8(int8_t a, int8_t b, int8_t *r) { return __builtin_mul_overflow(a, b, r); }
__attribute__((noinline)) static int add_u16(uint16_t a, uint16_t b, uint16_t *r) { return __builtin_add_overflow(a, b, r); }
__attribute__((noinline)) static int sub_u16(uint16_t a, uint16_t b, uint16_t *r) { return __builtin_sub_overflow(a, b, r); }
__attribute__((noinline)) static int mul_u16(uint16_t a, uint16_t b, uint16_t *r) { return __builtin_mul_overflow(a, b, r); }
__attribute__((noinline)) static int add_i16(int16_t a, int16_t b, int16_t *r) { return __builtin_add_overflow(a, b, r); }
__attribute__((noinline)) static int sub_i16(int16_t a, int16_t b, int16_t *r) { return __builtin_sub_overflow(a, b, r); }
__attribute__((noinline)) static int mul_i16(int16_t a, int16_t b, int16_t *r) { return __builtin_mul_overflow(a, b, r); }
__attribute__((noinline)) static int add_u32(uint32_t a, uint32_t b, uint32_t *r) { return __builtin_add_overflow(a, b, r); }
__attribute__((noinline)) static int sub_u32(uint32_t a, uint32_t b, uint32_t *r) { return __builtin_sub_overflow(a, b, r); }
__attribute__((noinline)) static int mul_u32(uint32_t a, uint32_t b, uint32_t *r) { return __builtin_mul_overflow(a, b, r); }
__attribute__((noinline)) static int add_i32(int32_t a, int32_t b, int32_t *r) { return __builtin_add_overflow(a, b, r); }
__attribute__((noinline)) static int sub_i32(int32_t a, int32_t b, int32_t *r) { return __builtin_sub_overflow(a, b, r); }
__attribute__((noinline)) static int mul_i32(int32_t a, int32_t b, int32_t *r) { return __builtin_mul_overflow(a, b, r); }
__attribute__((noinline)) static int add_mix_u32(int32_t a, int32_t b, uint32_t *r) { return __builtin_add_overflow(a, b, r); }
__attribute__((noinline)) static int sub_mix_i32(uint16_t a, uint16_t b, int32_t *r) { return __builtin_sub_overflow(a, b, r); }

int main(void)
{
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint8_t a = (uint8_t)vals[i], b = (uint8_t)vals[j], r;
      int o = add_u8(a, b, &r);
      int64_t full = (int64_t)a + (int64_t)b;
      uint8_t want = (uint8_t)full;
      int want_o = full < 0LL || full > 255LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint8_t a = (uint8_t)vals[i], b = (uint8_t)vals[j], r;
      int o = sub_u8(a, b, &r);
      int64_t full = (int64_t)a - (int64_t)b;
      uint8_t want = (uint8_t)full;
      int want_o = full < 0LL || full > 255LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint8_t a = (uint8_t)vals[i], b = (uint8_t)vals[j], r;
      int o = mul_u8(a, b, &r);
      int64_t full = (int64_t)a * (int64_t)b;
      uint8_t want = (uint8_t)full;
      int want_o = full < 0LL || full > 255LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int8_t a = (int8_t)vals[i], b = (int8_t)vals[j], r;
      int o = add_i8(a, b, &r);
      int64_t full = (int64_t)a + (int64_t)b;
      int8_t want = (int8_t)full;
      int want_o = full < -128LL || full > 127LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int8_t a = (int8_t)vals[i], b = (int8_t)vals[j], r;
      int o = sub_i8(a, b, &r);
      int64_t full = (int64_t)a - (int64_t)b;
      int8_t want = (int8_t)full;
      int want_o = full < -128LL || full > 127LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int8_t a = (int8_t)vals[i], b = (int8_t)vals[j], r;
      int o = mul_i8(a, b, &r);
      int64_t full = (int64_t)a * (int64_t)b;
      int8_t want = (int8_t)full;
      int want_o = full < -128LL || full > 127LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint16_t a = (uint16_t)vals[i], b = (uint16_t)vals[j], r;
      int o = add_u16(a, b, &r);
      int64_t full = (int64_t)a + (int64_t)b;
      uint16_t want = (uint16_t)full;
      int want_o = full < 0LL || full > 65535LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint16_t a = (uint16_t)vals[i], b = (uint16_t)vals[j], r;
      int o = sub_u16(a, b, &r);
      int64_t full = (int64_t)a - (int64_t)b;
      uint16_t want = (uint16_t)full;
      int want_o = full < 0LL || full > 65535LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint16_t a = (uint16_t)vals[i], b = (uint16_t)vals[j], r;
      int o = mul_u16(a, b, &r);
      int64_t full = (int64_t)a * (int64_t)b;
      uint16_t want = (uint16_t)full;
      int want_o = full < 0LL || full > 65535LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int16_t a = (int16_t)vals[i], b = (int16_t)vals[j], r;
      int o = add_i16(a, b, &r);
      int64_t full = (int64_t)a + (int64_t)b;
      int16_t want = (int16_t)full;
      int want_o = full < -32768LL || full > 32767LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int16_t a = (int16_t)vals[i], b = (int16_t)vals[j], r;
      int o = sub_i16(a, b, &r);
      int64_t full = (int64_t)a - (int64_t)b;
      int16_t want = (int16_t)full;
      int want_o = full < -32768LL || full > 32767LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int16_t a = (int16_t)vals[i], b = (int16_t)vals[j], r;
      int o = mul_i16(a, b, &r);
      int64_t full = (int64_t)a * (int64_t)b;
      int16_t want = (int16_t)full;
      int want_o = full < -32768LL || full > 32767LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint32_t a = (uint32_t)vals[i], b = (uint32_t)vals[j], r;
      int o = add_u32(a, b, &r);
      int64_t full = (int64_t)a + (int64_t)b;
      uint32_t want = (uint32_t)full;
      int want_o = full < 0LL || full > 4294967295LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint32_t a = (uint32_t)vals[i], b = (uint32_t)vals[j], r;
      int o = sub_u32(a, b, &r);
      int64_t full = (int64_t)a - (int64_t)b;
      uint32_t want = (uint32_t)full;
      int want_o = full < 0LL || full > 4294967295LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint32_t a = (uint32_t)vals[i], b = (uint32_t)vals[j], r;
      int o = mul_u32(a, b, &r);
      uint64_t full = (uint64_t)a * (uint64_t)b; /* up to ~2^64: not an int64_t */
      uint32_t want = (uint32_t)full;
      int want_o = full > 4294967295ULL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int32_t a = (int32_t)vals[i], b = (int32_t)vals[j], r;
      int o = add_i32(a, b, &r);
      int64_t full = (int64_t)a + (int64_t)b;
      int32_t want = (int32_t)full;
      int want_o = full < -2147483648LL || full > 2147483647LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int32_t a = (int32_t)vals[i], b = (int32_t)vals[j], r;
      int o = sub_i32(a, b, &r);
      int64_t full = (int64_t)a - (int64_t)b;
      int32_t want = (int32_t)full;
      int want_o = full < -2147483648LL || full > 2147483647LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int32_t a = (int32_t)vals[i], b = (int32_t)vals[j], r;
      int o = mul_i32(a, b, &r);
      int64_t full = (int64_t)a * (int64_t)b;
      int32_t want = (int32_t)full;
      int want_o = full < -2147483648LL || full > 2147483647LL;
      if (o != want_o || r != want) bad++;
      mix((uint32_t)r); mix((uint32_t)o);
    }
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      int32_t a = (int32_t)vals[i], b = (int32_t)vals[j];
      uint32_t r;
      int o = add_mix_u32(a, b, &r);
      int64_t full = (int64_t)a + b;
      if (o != (full < 0 || full > 0xffffffffLL) || r != (uint32_t)full) bad++;
      uint16_t c = (uint16_t)vals[i], d = (uint16_t)vals[j];
      int32_t s;
      int p = sub_mix_i32(c, d, &s);
      if (p != 0 || s != (int32_t)c - (int32_t)d) bad++;
      mix(r); mix((uint32_t)s);
    }
  /* The result pointer aliases an operand: the check must use the operand's
   * value from before the store (gcc PR85095, PR108789). */
  for (int i = 0; i < NV; i++)
    for (int j = 0; j < NV; j++)
    {
      uint32_t x = (uint32_t)vals[i];
      int o = add_u32(x, (uint32_t)vals[j], &x);
      if (o != ((uint64_t)(uint32_t)vals[i] + (uint32_t)vals[j] > 0xffffffffULL)) bad++;
      int32_t y = (int32_t)vals[i];
      int p = sub_i32(y, (int32_t)vals[j], &y);
      int64_t fy = (int64_t)(int32_t)vals[i] - (int32_t)vals[j];
      if (p != (fy < -0x80000000LL || fy > 0x7fffffffLL)) bad++;
      uint16_t z = (uint16_t)vals[i];
      int q = mul_u16(z, (uint16_t)vals[j], &z);
      if (q != ((uint32_t)(uint16_t)vals[i] * (uint16_t)vals[j] > 0xffff)) bad++;
      mix(x); mix((uint32_t)y); mix(z);
    }
  printf("bad %u hash %08x\n", bad, (unsigned)h);
  return 0;
}
