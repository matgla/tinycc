/*
 *  Regression test for big-endian scalar_storage_order runtime accesses.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* scalar_storage_order("big-endian") members must be byte-swapped on every
 * runtime load and store, not only in static initializer images.
 *
 * tcc laid the bitfields out at big-endian bit positions but read and wrote
 * every member little-endian: `s.h = 0x1234` stored 34 12.  A member access of
 * a big-endian struct now marks the lvalue byte-reversed; gv() swaps after the
 * load, vstore() before the store, and initializers swap each scalar leaf.
 * The little-endian struct and the plain struct pin the unswapped layout. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>

typedef struct __attribute__((scalar_storage_order("big-endian")))
{
  unsigned short h;
  short sh;
  unsigned int i;
  signed char c;
  unsigned long long ll;
  float f;
  double d;
  int *p;
  unsigned int arr[3];
  unsigned short m[2][2];
} BE;

typedef struct __attribute__((scalar_storage_order("big-endian")))
{
  unsigned a : 4, b : 28;
  unsigned short x : 3, y : 13;
} BEBits;

typedef struct __attribute__((scalar_storage_order("little-endian")))
{
  unsigned short h;
  unsigned int i;
} LE;

typedef union __attribute__((scalar_storage_order("big-endian")))
{
  unsigned int w;
  unsigned short h;
} BEU;

typedef struct
{
  unsigned short h;
  BEBits bits;
} Outer;

static void dump(const char *tag, const void *p, int n)
{
  const unsigned char *b = p;
  printf("%s:", tag);
  for (int k = 0; k < n; k++)
    printf(" %02x", b[k]);
  printf("\n");
}

static BE g = {0x1234, -2, 0x11223344, 5, 0x0102030405060708ULL, 1.5f, -2.25, 0, {0xa1a2a3a4, 2, 3}, {{0x1122, 0x3344}, {5, 6}}};
static BEBits gb = {1, 2, 5, 0x123};
static LE gl = {0x1234, 0x11223344};

typedef struct __attribute__((scalar_storage_order("big-endian")))
{
  unsigned (*fn)(unsigned);
  unsigned short tag;
  BEBits bits;
  unsigned vals[4];
} BEMore;

__attribute__((noinline)) static unsigned take(unsigned v) { return v * 3u; }
__attribute__((noinline)) static unsigned take_u(unsigned v) { return v + 1u; }
__attribute__((noinline)) static BE make(unsigned v)
{
  BE r;
  memset(&r, 0, sizeof r);
  r.i = v;
  r.ll = (unsigned long long)v << 32 | 0xa5;
  return r;
}

static BEMore gm = {0, .tag = 0x0102, .bits = {.b = 0x345}, .vals = {[1 ... 2] = 0x10203040}};
static Outer go = {.h = 0x5566, .bits.y = 0x2ab};
__attribute__((noinline)) static unsigned get_i(BE *s) { return s->i; }
__attribute__((noinline)) static void set_i(BE *s, unsigned v) { s->i = v; }
__attribute__((noinline)) static int get_sh(const BE *s) { return s->sh; }

int main(void)
{
  volatile unsigned vh = 0xbeef, vi = 0xcafef00d;
  int target[2] = {77, 88};
  BE s;
  memset(&s, 0, sizeof s);

  /* static initializer image and reads from it */
  dump("g.h", (const char *)&g + offsetof(BE, h), 2);
  dump("g.i", (const char *)&g + offsetof(BE, i), 4);
  dump("g.ll", (const char *)&g + offsetof(BE, ll), 8);
  dump("g.f", (const char *)&g + offsetof(BE, f), 4);
  dump("g.d", (const char *)&g + offsetof(BE, d), 8);
  dump("g.arr", g.arr, 12);
  dump("g.m", g.m, 8);
  printf("g: %x %d %x %d %llx %d %d %x %x %x\n", g.h, g.sh, g.i, g.c, g.ll, (int)(g.f * 100), (int)(g.d * 100),
         g.arr[0], g.m[0][1], g.m[1][0]);

  /* runtime stores, then the memory image and the values read back */
  s.h = 0x1234;
  s.sh = -3;
  s.i = 0x11223344;
  s.c = -7;
  s.ll = 0x0102030405060708ULL;
  s.f = 2.5f;
  s.d = -0.75;
  s.p = target;
  for (int k = 0; k < 3; k++)
    s.arr[k] = 0x01020304u * (k + 1);
  s.m[1][1] = 0xabcd;
  dump("s.h", (const char *)&s + offsetof(BE, h), 2);
  dump("s.sh", (const char *)&s + offsetof(BE, sh), 2);
  dump("s.i", (const char *)&s + offsetof(BE, i), 4);
  dump("s.ll", (const char *)&s + offsetof(BE, ll), 8);
  dump("s.f", (const char *)&s + offsetof(BE, f), 4);
  dump("s.d", (const char *)&s + offsetof(BE, d), 8);
  dump("s.arr", s.arr, 12);
  dump("s.m", s.m, 8);
  printf("s: %x %d %x %d %llx %d %d %d %d %x %x\n", s.h, s.sh, s.i, s.c, s.ll, (int)(s.f * 100), (int)(s.d * 100),
         *s.p, s.p[1], s.arr[2], s.m[1][1]);

  /* read-modify-write, expression values, conversions */
  s.h += 0x0101;
  s.i++;
  ++s.sh;
  s.ll <<= 4;
  s.arr[1] |= 0xff000000u;
  unsigned chain = s.i = 0x55667788;
  int neg = s.sh = -300;
  dump("rmw.h", (const char *)&s + offsetof(BE, h), 2);
  dump("rmw.i", (const char *)&s + offsetof(BE, i), 4);
  dump("rmw.sh", (const char *)&s + offsetof(BE, sh), 2);
  printf("rmw: %x %x %d %llx %x chain=%x neg=%d\n", s.h, s.i, s.sh, s.ll, s.arr[1], chain, neg);
  printf("conv: %x %d %x %u\n", (unsigned char)s.i, (signed char)s.h, (unsigned short)s.i, take(s.h));
  printf("cmp: %d %d %d\n", s.i == 0x55667788, s.h > 0x1300, s.sh < -299);
  switch (s.h)
  {
  case 0x1335:
    printf("switch ok\n");
    break;
  default:
    printf("switch bad %x\n", s.h);
  }
  printf("ternary: %x\n", s.sh < 0 ? s.h : s.i);

  /* through pointers and volatile values */
  set_i(&s, vi);
  s.h = vh;
  dump("ptr.i", (const char *)&s + offsetof(BE, i), 4);
  dump("ptr.h", (const char *)&s + offsetof(BE, h), 2);
  printf("ptr: %x %x %d\n", get_i(&s), s.h, get_sh(&s));

  /* a struct copy moves the big-endian bytes unchanged */
  BE t = s;
  dump("copy.i", (const char *)&t + offsetof(BE, i), 4);
  printf("copy: %x\n", t.i);

  /* local initializers, constant and runtime-valued */
  BE l = {0x2468, 1, vi, 0, 0x1122334455667788ULL};
  dump("l.h", (const char *)&l + offsetof(BE, h), 2);
  dump("l.i", (const char *)&l + offsetof(BE, i), 4);
  dump("l.ll", (const char *)&l + offsetof(BE, ll), 8);
  printf("l: %x %d %x %llx\n", l.h, l.sh, l.i, l.ll);

  /* bitfields: static image, runtime stores, reads */
  BEBits bf;
  memset(&bf, 0, sizeof bf);
  bf.a = 1;
  bf.b = 2;
  bf.x = 5;
  bf.y = 0x123;
  dump("gb", &gb, sizeof gb);
  dump("bf", &bf, sizeof bf);
  bf.b += 0x100;
  bf.a = 0xf;
  dump("bf2", &bf, sizeof bf);
  printf("bf: %x %x %x %x / %x %x %x %x\n", bf.a, bf.b, bf.x, bf.y, gb.a, gb.b, gb.x, gb.y);
  BEBits lb = {3, 0xabcdef, 2, 0x1fff};
  dump("lb", &lb, sizeof lb);
  printf("lb: %x %x %x %x\n", lb.a, lb.b, lb.x, lb.y);

  /* a big-endian struct nested in a plain one keeps its own order */
  Outer o;
  memset(&o, 0, sizeof o);
  o.h = 0x1234;
  o.bits.b = 0x777;
  dump("outer", &o, sizeof o);
  printf("outer: %x %x\n", o.h, o.bits.b);

  /* a big-endian union */
  BEU u;
  u.w = 0x0a0b0c0d;
  dump("u", &u, sizeof u);
  printf("u: %x %x\n", u.w, u.h);

  /* conditions, unary operators, same-type arguments, float read-modify-write */
  s.i = 0x80000001u;
  s.h = 0;
  s.f = 1.25f;
  s.d = 3.0;
  if (s.i)
    printf("if ok\n");
  printf("unary: %d %d %x %x %d %d\n", !s.h, !s.i, ~s.i, -s.i, s.i && s.h, s.i || s.h);
  s.f += 0.5f;
  s.d *= 2.0;
  s.ll = 0x00000001ffffffffULL;
  printf("rmw2: %d %d %d %u\n", (int)(s.f * 100), (int)s.d, s.ll > 0xffffffffULL, take_u(s.i));
  dump("rmw2.f", (const char *)&s + offsetof(BE, f), 4);
  int n = 0;
  for (s.h = 0; s.h < 3; s.h++)
    n += s.h;
  printf("loop: %d %x\n", n, s.h);
  printf("make: %x %llx\n", make(0x12345678).i, make(7).ll);
  volatile BE *vp = &s;
  vp->i = 0xdeadbeef;
  printf("volatile: %x\n", vp->i);

  /* designated / range initializers and a function pointer member */
  gm.fn = take_u;
  dump("gm.tag", (const char *)&gm + offsetof(BEMore, tag), 2);
  dump("gm.bits", &gm.bits, sizeof gm.bits);
  dump("gm.vals", gm.vals, sizeof gm.vals);
  dump("go", &go, sizeof go);
  printf("gm: %u %x %x %x %x\n", gm.fn(41), gm.tag, gm.bits.b, gm.vals[2], go.bits.y);
  BEMore lm = {take, .vals = {[0 ... 1] = vi, 9}};
  lm.tag = lm.vals[2] + 1;
  dump("lm.vals", lm.vals, sizeof lm.vals);
  printf("lm: %u %x %x\n", lm.fn(5), lm.tag, lm.vals[1]);

  /* little-endian scalar_storage_order is the native order */
  LE le;
  memset(&le, 0, sizeof le);
  le.h = 0x1234;
  le.i = 0x11223344;
  dump("le", &le, sizeof le);
  dump("gl", &gl, sizeof gl);
  printf("le: %x %x %x\n", le.h, le.i, gl.i);
  return 0;
}
