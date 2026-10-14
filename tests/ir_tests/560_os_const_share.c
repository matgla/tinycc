/* -Os os_const_share: a wide constant stored, passed and copied at several
   sites is built once at entry into a temp that every site reads.

   The shapes are the Zig C backend's: 0xaaaaaaaa (`undefined`) written into
   each error path's payload, passed as undefined call arguments, and merged
   through a phi; -1 and 0x2002 beside it (a function shares up to four).
   pressure() keeps more values live across its calls than there are
   callee-saved registers, so the shared temp is spilled and must be RELOADED
   at its uses (it is never rematerialised); again() starts with a label a
   backward goto targets, so the entry def goes in front of a jump target. */
#include <stdio.h>
#include <stdint.h>

struct pl
{
  uint32_t a, b, c;
};
struct eu
{
  struct pl payload;
  uint16_t error;
};

static int calls;

__attribute__((noinline)) uint16_t step(int x)
{
  calls++;
  return (uint16_t)(x & 3 ? 0 : x + 1);
}

__attribute__((noinline)) uint32_t take(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
  return a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ e;
}

__attribute__((noinline)) struct eu fallible(int x)
{
  struct eu r;
  uint16_t e = step(x);
  if (e)
  {
    r.payload = (struct pl){0xaaaaaaaau, 0xaaaaaaaau, 0xffffffffu};
    r.error = e;
    return r;
  }
  uint32_t t = take(0xaaaaaaaau, 1, 0xaaaaaaaau, 0x2002u, 0xffffffffu);
  e = step(x + 1);
  if (e)
  {
    r.payload = (struct pl){0xaaaaaaaau, t, 0x2002u};
    r.error = e;
    return r;
  }
  e = step(x + 2);
  if (e)
  {
    r.payload = (struct pl){0xaaaaaaaau, 0xaaaaaaaau, 0x2002u};
    r.error = e;
    return r;
  }
  r.payload = (struct pl){(uint32_t)x, t, 0x2002u};
  r.error = 0;
  return r;
}

/* The phi: v is 0xaaaaaaaa on the error paths, a real value otherwise. */
__attribute__((noinline)) uint32_t merge(int x)
{
  uint32_t v;
  if (step(x))
    v = 0xaaaaaaaau;
  else if (step(x + 1))
    v = 0xaaaaaaaau;
  else if (step(x + 2))
    v = 0xffffffffu;
  else
    v = (uint32_t)x * 7u;
  return v ^ (step(x + 3) ? 0xaaaaaaaau : 0xffffffffu);
}

__attribute__((noinline)) uint32_t pressure(const uint32_t *in, uint32_t *out, int rounds)
{
  uint32_t a = in[0], b = in[1], c = in[2], d = in[3], e = in[4], f = in[5], g = in[6], h = in[7];
  uint32_t i = in[8], j = in[9], k = in[10], l = in[11], m = in[12], n = in[13];
  out[0] = 0xaaaaaaaau;
  out[1] = take(0xaaaaaaaau, 1, 2, 3, 0xaaaaaaaau);
  for (int r = 0; r < rounds; r++)
  {
    a += take(a, b, c, d, e) * 3 + n;
    b += take(e, f, g, h, i) * 5 + m;
    c += take(i, j, k, l, m) * 7 + l;
    d += take(m, n, a, b, c) * 9 + k;
    e += take(a, c, e, g, i) * 11 + j;
    f += take(k, m, b, d, f) * 13 + i;
    g += take(h, j, l, n, a) * 15 + h;
  }
  out[2] = 0xaaaaaaaau;
  out[3] = take(a, 0xaaaaaaaau, b, 0xaaaaaaaau, c);
  out[4] = 0xaaaaaaaau;
  return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j ^ k ^ l ^ m ^ n;
}

__attribute__((noinline)) uint32_t again(int n, uint32_t *out)
{
  uint32_t acc = 0;
top:
  out[n & 3] = 0xaaaaaaaau;
  acc += take(0xaaaaaaaau, (uint32_t)n, 0xaaaaaaaau, 0, 0xaaaaaaaau);
  if (--n > 0)
    goto top;
  return acc;
}

int main(void)
{
  for (int x = 0; x < 6; x++)
  {
    struct eu r = fallible(x);
    printf("fallible(%d): err=%u %08x %08x %08x\n", x, r.error, (unsigned)r.payload.a, (unsigned)r.payload.b,
           (unsigned)r.payload.c);
    printf("merge(%d)=%08x\n", x, (unsigned)merge(x));
  }
  uint32_t in[14], out[5];
  for (int k = 0; k < 14; k++)
    in[k] = 0x01010101u * (uint32_t)(k + 1);
  uint32_t p = pressure(in, out, 3);
  printf("pressure=%08x out=%08x %08x %08x %08x %08x\n", (unsigned)p, (unsigned)out[0], (unsigned)out[1],
         (unsigned)out[2], (unsigned)out[3], (unsigned)out[4]);
  uint32_t o4[4] = {0, 0, 0, 0};
  uint32_t ag = again(5, o4);
  printf("again=%08x %08x %08x %08x %08x calls=%d\n", (unsigned)ag, (unsigned)o4[0], (unsigned)o4[1],
         (unsigned)o4[2], (unsigned)o4[3], calls);
  return 0;
}
