/* Fields of a small frame object accessed in part: SRA makes the widest
   access at an offset the field and rewrites the others as extracts and
   inserts of its VAR.  The Zig C backend writes an error union's `u16 error`
   by itself and copies the union a word at a time; an optional of bytes is
   built one byte at a time and returned as a word; a result buffer is read
   at two widths after the call. */
#include <stdio.h>

struct eu
{
  unsigned payload;
  unsigned short err;
  unsigned short pad;
};

struct opt
{
  unsigned char val;
  unsigned char is_null;
  signed char delta;
  unsigned char tag;
};

struct pair
{
  unsigned lo, hi;
};

__attribute__((noinline)) struct eu make_eu(unsigned p, unsigned e)
{
  struct eu r;
  r.payload = p;
  r.err = (unsigned short)e;
  r.pad = 0;
  return r;
}

__attribute__((noinline)) struct pair make_pair(unsigned a, unsigned b)
{
  struct pair r;
  r.lo = a;
  r.hi = b;
  return r;
}

__attribute__((noinline)) unsigned word_of(struct opt o)
{
  return o.val | (o.is_null << 8) | ((unsigned)(unsigned char)o.delta << 16) | ((unsigned)o.tag << 24);
}

__attribute__((noinline)) unsigned two_widths(unsigned p, unsigned e, unsigned k)
{
  struct eu t = make_eu(p, e); /* the whole union copied in, a word at a time */
  unsigned r = t.payload;
  if (k & 1)
    t.err = (unsigned short)(k * 7919); /* the u16 by itself: a partial store */
  if (k & 2)
    t.err = k; /* a value wider than the field: masked */
  r += t.err;  /* the u16 read back */
  r ^= *(unsigned *)&t.err; /* the word holding it read whole */
  return r;
}

__attribute__((noinline)) unsigned bytes_in_word(unsigned k)
{
  struct opt o;
  *(unsigned *)&o = 0x11223344u; /* the word written whole */
  o.val = (unsigned char)k;      /* then its bytes */
  o.is_null = (k & 1);
  o.delta = (signed char)(k * 37);
  if (k & 4)
    o.tag = 0xEE;
  int d = o.delta; /* a signed byte read out of the word */
  unsigned r = word_of(o) + (unsigned)d;
  r += o.is_null;
  r += o.tag;
  return r;
}

__attribute__((noinline)) unsigned buffer_two_widths(unsigned a, unsigned b)
{
  struct pair p = make_pair(a, b); /* a result buffer, then read at two widths */
  unsigned r = p.lo;
  r += *(unsigned short *)&p.hi;
  r += *((unsigned char *)&p.hi + 1);
  r += p.hi;
  return r;
}

struct ev
{
  unsigned short err;
};

__attribute__((noinline)) void dirty(void)
{
  volatile unsigned junk[64];
  for (unsigned i = 0; i < 64; i++)
    junk[i] = 0xDEADBEEFu + i;
}

/* Zig's `!void`: a two-byte struct zero-initialized a byte at a time, set
   whole on the error path, read whole and tested.  The field's VAR is only
   ever written in part on the success path, so its upper bits are whatever
   the register held: the read must mask them. */
__attribute__((noinline)) unsigned byte_init_then_test(unsigned k)
{
  struct ev e;
  dirty();
  if (k & 1)
    e.err = (unsigned short)(k * 3);
  else
  {
    ((unsigned char *)&e)[0] = 0;
    ((unsigned char *)&e)[1] = 0;
  }
  struct ev f = e;
  if (f.err)
    return 1000 + f.err;
  return 7;
}

int main(void)
{
  unsigned total = 0;
  for (unsigned k = 0; k < 8; k++)
  {
    unsigned a = two_widths(k * 1000003u, k * 5 + 1, k);
    unsigned b = bytes_in_word(k);
    unsigned c = buffer_two_widths(k * 77777u, k * 0x01010101u + 0x0badu);
    unsigned d = byte_init_then_test(k);
    printf("k=%u a=%u b=%u c=%u d=%u\n", k, a, b, c, d);
    total += a ^ b ^ c ^ d;
  }
  printf("total=%u\n", total);
  return 0;
}
