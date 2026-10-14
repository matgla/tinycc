/*
 *  TCC IR - `return try f()` forwards a call's result into the return buffer
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* sret_nrvo: a local error union whose payload alone is copied into the
 * returned one (the Zig C backend's `t9 = t5.payload; t8.payload = t9;`) was
 * never merged into *P0, so every `return try` copied 160 bytes.  It now lives
 * in *P0 when its bytes there are dead wherever it is written, word by word;
 * the shapes where a word of it is still read after the other is written stay
 * apart. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

struct Hdr
{
  uint32_t w[40];
};
struct EHdr
{
  struct Hdr payload;
  uint16_t error;
};
struct Outer
{
  uint32_t tag;
  struct Hdr h;
};

static unsigned sum(const struct Hdr *h)
{
  unsigned s = 0;
  for (int i = 0; i < 40; i++)
    s = s * 31 + h->w[i];
  return s;
}

static void show(const char *tag, const struct EHdr *e)
{
  printf("%s err=%u w0=%u w3=%u w4=%u w39=%u sum=%u\n", tag, e->error, e->payload.w[0], e->payload.w[3],
         e->payload.w[4], e->payload.w[39], sum(&e->payload));
}

static const struct Hdr *watched;
static unsigned seen;

__attribute__((noinline)) static struct EHdr make_header(const struct Hdr *src, int32_t off)
{
  struct EHdr r;
  if (off < 0)
  {
    memset(&r, 0, sizeof r);
    r.error = 7;
    return r;
  }
  for (int i = 0; i < 40; i++)
    r.payload.w[i] = src->w[i] + (uint32_t)off * 3 + i;
  if (watched)
    seen = watched->w[0] + watched->w[1];
  r.error = 0;
  return r;
}

/* the Zig C spelling of `return try make_header(src, off);` */
__attribute__((noinline)) static struct EHdr forward(const struct Hdr *src, int32_t off)
{
  struct EHdr t5, t8;
  struct Hdr t9;
  uint16_t t6, t7;
  t5 = make_header(src, off);
  if (t5.error)
  {
    t6 = t5.error;
    t7 = t6;
    t8.payload = (struct Hdr){{0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau,
                               0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau,
                               0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau,
                               0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau,
                               0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau,
                               0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau,
                               0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau}};
    t8.error = t7;
    return t8;
  }
  t9 = t5.payload;
  t8.payload = t9;
  t8.error = UINT16_C(0);
  return t8;
}

/* one more `return try` level over it, as create_file_header over
 * create_file_header_with_offset */
__attribute__((noinline)) static struct EHdr forward2(const struct Hdr *src, int32_t off)
{
  struct EHdr t5, t8;
  struct Hdr t9;
  t5 = forward(src, off);
  if (t5.error)
  {
    memset(&t8.payload, 0xaa, sizeof t8.payload);
    t8.error = t5.error;
    return t8;
  }
  t9 = t5.payload;
  t8.payload = t9;
  t8.error = 0;
  return t8;
}

/* t5's word 3 is read after t8's word 3 is written: the two cannot share */
__attribute__((noinline)) static struct EHdr late_read(const struct Hdr *src, int32_t off)
{
  struct EHdr t5, t8;
  struct Hdr t9;
  t5 = make_header(src, off);
  if (t5.error)
  {
    memset(&t8, 0, sizeof t8);
    t8.error = t5.error;
    return t8;
  }
  t9 = t5.payload;
  t8.payload = t9;
  t8.payload.w[3] = 77;
  t8.error = 0;
  t8.payload.w[4] = t5.payload.w[3];
  return t8;
}

/* t5's error word is read after t8's error is stored */
__attribute__((noinline)) static struct EHdr late_error_read(const struct Hdr *src, int32_t off)
{
  struct EHdr t5, t8;
  struct Hdr t9;
  t5 = make_header(src, off);
  t9 = t5.payload;
  t8.payload = t9;
  t8.error = 3;
  t8.payload.w[39] = t5.error + 100u;
  return t8;
}

/* the payload goes 4 bytes into the result: t5 would not fit in it there */
__attribute__((noinline)) static struct Outer shifted(const struct Hdr *src, int32_t off)
{
  struct EHdr t5;
  struct Outer t8;
  struct Hdr t9;
  t5 = make_header(src, off);
  t9 = t5.payload;
  t8.h = t9;
  t8.tag = t5.error + 5u;
  return t8;
}

/* written in a loop: an earlier iteration's t8 is still read after t5 is written */
__attribute__((noinline)) static struct EHdr looped(const struct Hdr *src, int n)
{
  struct EHdr t5, t8;
  memset(&t8, 0, sizeof t8);
  for (int i = 0; i < n; i++)
  {
    t5 = make_header(src, i);
    t5.payload.w[1] += t8.payload.w[0];
    t8.payload = t5.payload;
    t8.error = 0;
  }
  return t8;
}

int main(void)
{
  struct Hdr src;
  for (int i = 0; i < 40; i++)
    src.w[i] = 1000u + i * 7u;

  struct EHdr r = forward(&src, 5);
  show("forward ok", &r);
  r = forward(&src, -1);
  show("forward err", &r);
  r = forward2(&src, 9);
  show("forward2 ok", &r);
  r = forward2(&src, -2);
  show("forward2 err", &r);
  r = late_read(&src, 4);
  show("late_read", &r);
  r = late_error_read(&src, 2);
  show("late_error_read", &r);
  r = late_error_read(&src, -3);
  show("late_error_read err", &r);
  struct Outer o = shifted(&src, 6);
  printf("shifted tag=%u w0=%u w39=%u sum=%u\n", o.tag, o.h.w[0], o.h.w[39], sum(&o.h));
  r = looped(&src, 4);
  show("looped", &r);

  /* the destination is also what make_header reads (src) and watches: the
   * caller must not hand it over as the result buffer */
  struct EHdr g;
  for (int i = 0; i < 40; i++)
    g.payload.w[i] = 50u + i;
  g.error = 0;
  watched = &g.payload;
  g = forward2(&g.payload, 1);
  show("aliased", &g);
  printf("seen=%u\n", seen);
  watched = 0;
  return 0;
}
