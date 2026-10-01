/* SRA: a u16 stored into the low half of a word field whose other half is
 * struct padding becomes a plain move of the value -- the padding may hold
 * anything after a member store (C11 6.2.6.1p6).  Zig's error unions have
 * exactly this shape: `{ payload; u16 error; }` returned through a result
 * pointer, the error written by itself and the struct copied whole.  The
 * values stored carry high garbage bits, so a read that forgot to mask the
 * field would show them. */
#include <stdint.h>
#include <stdio.h>

typedef struct
{
  uint32_t payload;
  uint16_t err; /* bytes 6-7: padding */
} EU;

/* The u16 at byte 6 over an unnamed bit-field, which counts as a member:
 * its store stays an insert that keeps bytes 4-5. */
typedef struct
{
  uint32_t payload;
  unsigned : 16;
  uint16_t hi;
} EH;

static volatile uint32_t seed = 0x12345678u;

__attribute__((noinline)) EU make(uint32_t v, uint32_t e)
{
  EU r;
  r.payload = v * 0x9E3779B9u;
  r.err = (uint16_t)(e ^ (v >> 7));
  return r;
}

/* Zig's `if (t) |err| return err; ... r.err = ...`: the u16 of a call's
 * result updated in place, the struct returned whole. */
__attribute__((noinline)) EU check(uint32_t v, uint32_t e)
{
  EU t = make(v, e);
  EU r;
  if (t.err & 1)
  {
    r.err = t.err;
    r.payload = 0xAAAAAAAAu;
    return r;
  }
  r = make(e, v);
  r.err = (uint16_t)(r.err + v + 0x7FFF0000u);
  return r;
}

__attribute__((noinline)) void update(EU *out, uint32_t v, uint32_t e)
{
  EU t = make(v, e);
  if (t.err & 2)
    t.err = (uint16_t)((e >> 1) | 0xFFFF0000u);
  else
    t.err = (uint16_t)(t.payload + e);
  *out = t;
}

__attribute__((noinline)) EH make_hi(uint32_t v, uint32_t h)
{
  EH r;
  r.payload = v;
  r.hi = (uint16_t)h;
  return r;
}

__attribute__((noinline)) EH pass_hi(uint32_t v, uint32_t k)
{
  EH r = make_hi(v, v * 3 + k);
  if (k & 1)
    r.hi = (uint16_t)(r.hi ^ (k | 0xFFFF0000u));
  return r;
}

int main(void)
{
  uint32_t sum = 0;
  for (uint32_t v = 0; v < 6; v++)
  {
    EU a = check(seed * (v + 1), v + 0x10000u);
    EU b;
    update(&b, seed + v, v * 0x01010101u);
    printf("%u: %08x %04x  %08x %04x\n", (unsigned)v, (unsigned)a.payload, (unsigned)a.err, (unsigned)b.payload,
           (unsigned)b.err);
    sum += a.payload ^ a.err ^ b.payload ^ b.err;
  }
  for (uint32_t k = 0; k < 4; k++)
  {
    EH h = pass_hi(seed + k, k + 0xABCD0000u);
    printf("hi %u: %08x %04x\n", (unsigned)k, (unsigned)h.payload, (unsigned)h.hi);
    sum += h.payload ^ h.hi;
  }
  printf("sum %08x\n", (unsigned)sum);
  return 0;
}
