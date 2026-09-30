/* By-value struct arguments of locals more than 32 KiB down the frame.  A
   struct stack operand keeps its offset in 16 bits, so tcc_ir_put names such
   an argument through its address (`T <- Addr[slot]; PARAM T***DEREF***`).
   SRA takes that form as it takes the slot itself: the argument becomes the
   field VARs, one per word.  The Zig C backend's slices and format options
   reach calls this way from its large frames.

   scribble() fills the stack first, so a word read from a slot nobody wrote
   is 0xA5A5A5A5 rather than a lucky leftover. */
#include <stdio.h>

struct slice
{
  const unsigned char *ptr;
  unsigned len;
};

struct opts
{
  unsigned char width;
  unsigned char fill;
  unsigned short prec;
  unsigned flags;
};

struct quad
{
  unsigned a, b, c, d;
};

__attribute__((noinline)) void scribble(void)
{
  volatile unsigned char junk[36000];
  for (unsigned i = 0; i < sizeof junk; i++)
    junk[i] = 0xA5;
}

__attribute__((noinline)) unsigned hash(struct slice s)
{
  unsigned h = s.len;
  for (unsigned i = 0; i < s.len; i++)
    h = h * 31 + s.ptr[i];
  return h;
}

__attribute__((noinline)) unsigned fmt(struct slice s, struct opts o)
{
  return hash(s) ^ o.width ^ (o.fill << 8) ^ ((unsigned)o.prec << 16) ^ o.flags;
}

__attribute__((noinline)) unsigned mix(unsigned x, struct quad q)
{
  return x * 3 + q.a + q.b * 5 + q.c * 7 + q.d * 11;
}

__attribute__((noinline)) unsigned run(const unsigned char *p, unsigned n, unsigned k)
{
  unsigned char buf[33000];
  struct slice s;
  struct opts o;
  struct quad q;
  for (unsigned i = 0; i < n; i++)
    buf[i] = p[i] + k;
  s.ptr = buf;
  s.len = n;
  o.width = k;
  o.fill = ' ';
  o.prec = k * 300;
  o.flags = n ^ 0x5a5a;
  unsigned r = hash(s);
  s.len = n - 1;
  r += fmt(s, o);
  q.a = r;
  q.b = k;
  q.c = n;
  q.d = r >> 3;
  r = mix(r, q);
  if (k & 1)
  {
    q.b = 99;
    s.ptr = p;
  }
  r += mix(r, q) + hash(s);
  return r + buf[n / 2];
}

int main(void)
{
  static const unsigned char text[] = "far struct arguments";
  unsigned total = 0;
  for (unsigned k = 0; k < 4; k++)
  {
    scribble();
    unsigned r = run(text, sizeof text - 1, k);
    printf("k=%u r=%u\n", k, r);
    total ^= r;
  }
  printf("total=%u\n", total);
  return 0;
}
