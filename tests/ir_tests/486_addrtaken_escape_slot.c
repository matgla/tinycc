/* An address-taken local whose address leaves through a call's result (or
   a store to memory) keeps its stack slot until the function ends.  The
   allocator followed the address only through copies and pointer arithmetic,
   so `t = as_bytes(&b)` ended b's lifetime at the `&b`, and a later
   address-taken local got the same slot while t still pointed into it: the
   memcpy through t copied the low byte of the pointer stored there.  Zig's
   autoHash of a u8 is this shape (mem.asBytes, then Wyhash.update), and the
   -O0-built Zig compiler hashed garbage and panicked `std.lang is corrupt`. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct W
{
  uint32_t total_len, buf_len;
  uint8_t buf[48];
};

struct slice
{
  const uint8_t *ptr;
  uintptr_t len;
};

__attribute__((noinline)) const uint8_t *as_bytes(const uint8_t *p) { return p; }

__attribute__((noinline)) void update(struct W *const a0, uint8_t const a1)
{
  struct W *t5, *t7;
  struct W *const *t8;
  const uint8_t *t3;
  struct slice t6, t9;
  struct slice const *t10;
  uint8_t t1;
  t1 = a1;
  t3 = as_bytes(&t1);
  t6.ptr = t3;
  t6.len = 1;
  t7 = a0;
  t8 = (struct W *const *)&t7;
  t9 = t6;
  t10 = &t9;
  t5 = *t8;
  t5->total_len += t10->len;
  if (t5->buf_len + t10->len <= 48)
  {
    memcpy(t5->buf + t5->buf_len, t6.ptr, t10->len);
    t5->buf_len += t10->len;
  }
}

static const uint8_t *volatile stash;

/* The same through a store: the address is parked in memory and read back. */
__attribute__((noinline)) unsigned via_store(unsigned k)
{
  uint8_t b;
  unsigned r;
  b = (uint8_t)k;
  stash = &b;
  unsigned *q;
  unsigned w = 0xdeadbeefu;
  q = &w;
  *q ^= k;
  r = *stash;
  return r + (w == (0xdeadbeefu ^ k) ? 0 : 1000);
}

int main(void)
{
  static struct W w;
  static const uint8_t in[] = {0x5a, 0x3c, 0x01, 0xff};
  for (unsigned i = 0; i < sizeof in; i++)
    update(&w, in[i]);
  printf("%02x %02x %02x %02x len=%u total=%u\n", w.buf[0], w.buf[1], w.buf[2], w.buf[3], (unsigned)w.buf_len,
         (unsigned)w.total_len);
  printf("%u %u\n", via_store(0x21), via_store(0x7e));
  return 0;
}
