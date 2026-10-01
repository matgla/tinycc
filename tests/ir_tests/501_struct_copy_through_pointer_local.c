/* A small aggregate copied from `*p`, where p is a pointer held in a local,
   is now copied inline like one through a parameter: the frontend had kept it
   a memmove because that deref still carried the local's frame offset in c.i,
   which is no displacement.  And a struct returned into a local through the
   hidden result pointer is no longer copied onto itself.  Copies from every
   byte offset, of byte arrays, words and a struct with a narrow member and
   padding, through pointer locals and into pointer locals. */
#include <stdio.h>
#include <string.h>

struct A2 { unsigned char b[2]; };
struct A4 { unsigned char b[4]; };
struct A7 { unsigned char b[7]; };
struct W3 { unsigned w[3]; };
struct M  { unsigned short h; unsigned char c; unsigned w; };
struct Big { unsigned long long q[12]; };

static unsigned char src[64] __attribute__((aligned(8))), dst[64] __attribute__((aligned(8)));

__attribute__((noinline)) unsigned sum4(int off)
{
  const unsigned char *p = src + off;
  const struct A4 *t20;
  struct A4 t21;
  t20 = (const struct A4 *)p;
  t21 = *t20;
  return t21.b[0] | t21.b[1] << 8 | t21.b[2] << 16 | (unsigned)t21.b[3] << 24;
}

__attribute__((noinline)) unsigned sum2_7(int off)
{
  const unsigned char *p = src + off;
  const struct A2 *a = (const struct A2 *)p;
  const struct A7 *b = (const struct A7 *)(p + 2);
  struct A2 x = *a;
  struct A7 y = *b;
  unsigned s = x.b[0] + x.b[1] * 3;
  for (int i = 0; i < 7; i++)
    s = s * 31 + y.b[i];
  return s;
}

__attribute__((noinline)) void put_w3(int off, struct W3 v)
{
  struct W3 *d = (struct W3 *)(dst + off);
  *d = v;
}

__attribute__((noinline)) struct M get_m(const struct M *pm)
{
  const struct M *q = pm;
  struct M m;
  m = *q;
  m.c++;
  return m;
}

__attribute__((noinline)) struct Big make_big(unsigned long long s)
{
  struct Big b;
  for (int i = 0; i < 12; i++)
    b.q[i] = s * (i + 1);
  return b;
}

__attribute__((noinline)) unsigned long long use_big(unsigned long long s)
{
  struct Big t3, t2;
  t3 = make_big(s);
  t2 = t3;
  t2.q[0] ^= 5;
  return t2.q[0] + t2.q[11] + t3.q[0];
}

int main(void)
{
  for (int i = 0; i < 64; i++)
    src[i] = (unsigned char)(i * 37 + 11);
  unsigned h = 0;
  for (int off = 0; off < 8; off++)
    h = h * 131 + sum4(off) + sum2_7(off);
  printf("reads %08x\n", h);

  struct W3 v = {{0x11223344u, 0x55667788u, 0x99aabbccu}};
  memset(dst, 0xee, sizeof dst);
  put_w3(8, v);
  h = 0;
  for (int i = 0; i < 24; i++)
    h = h * 33 + dst[i];
  printf("writes %08x\n", h);

  struct M m0 = {0xbeef, 7, 0xcafef00du};
  struct M m1 = get_m(&m0);
  printf("m %x %u %x\n", m1.h, m1.c, m1.w);
  printf("big %llx\n", use_big(0x123456789ull));
  return 0;
}
