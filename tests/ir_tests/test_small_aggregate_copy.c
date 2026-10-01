/* Small aggregate copies inlined as LOAD/STORE chunks instead of
 * __aeabi_memmove (vstore.c / small_aggregate_copy_plan).  Every shape the
 * plan accepts, between frame slots, globals and pointers, including an
 * overlapping copy that must keep memmove semantics. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

struct eu
{
  uint16_t error; /* Zig's `!void` */
};
struct two_bytes
{
  uint8_t a, b;
};
struct slice
{
  const char *ptr;
  uint32_t len;
};
struct opt_u16
{
  uint16_t payload;
  uint8_t is_null; /* 1 byte of padding follows */
};
union word_or_halves
{
  uint32_t w;
  uint16_t h[2];
};
struct nested
{
  struct two_bytes pair;
  uint16_t arr[1];
};

struct eu g_eu = {.error = 0x1234};
struct slice g_slice = {"hello", 5};

__attribute__((noinline)) static struct eu make_eu(int v)
{
  struct eu e = {.error = (uint16_t)v};
  return e;
}
__attribute__((noinline)) static void put_eu(struct eu *out, int v)
{
  struct eu t = make_eu(v);
  *out = t; /* frame slot -> pointer */
}
__attribute__((noinline)) static struct eu get_eu(const struct eu *in)
{
  struct eu t;
  t = *in; /* pointer -> frame slot */
  return t;
}
__attribute__((noinline)) static void copy_through(struct opt_u16 *dst, const struct opt_u16 *src)
{
  *dst = *src; /* pointer -> pointer */
}
__attribute__((noinline)) static unsigned bytes_of(struct two_bytes x)
{
  struct two_bytes y;
  y = x;
  return y.a * 256u + y.b;
}

int main(void)
{
  unsigned sum = 0;

  struct eu a, b;
  a = make_eu(7);
  b = a; /* frame slot -> frame slot */
  sum += b.error;

  struct eu out;
  put_eu(&out, 300);
  sum += get_eu(&out).error;

  struct eu from_global;
  from_global = g_eu; /* global -> frame slot */
  g_eu = make_eu(9);  /* frame slot -> global */
  sum += from_global.error + g_eu.error;

  struct slice s;
  s = g_slice;
  sum += s.len + (unsigned)(s.ptr[1] == 'e');

  struct opt_u16 o1 = {0xbeef, 1}, o2;
  copy_through(&o2, &o1);
  sum += o2.payload + o2.is_null;

  union word_or_halves u1, u2;
  u1.w = 0x00030004u;
  u2 = u1;
  sum += u2.h[0] + u2.h[1];

  struct nested n1 = {{1, 2}, {40}}, n2;
  n2 = n1;
  sum += n2.pair.a + n2.pair.b + n2.arr[0];

  sum += bytes_of((struct two_bytes){3, 4});

  /* Overlapping copy: shift two 2-byte structs down one slot. */
  struct eu arr[3] = {{1}, {2}, {3}};
  struct eu *p = &arr[0], *q = &arr[1];
  *p = *q;
  *q = arr[2];
  sum += arr[0].error * 100 + arr[1].error * 10 + arr[2].error;

  printf("%u\n", sum);
  return 0;
}
