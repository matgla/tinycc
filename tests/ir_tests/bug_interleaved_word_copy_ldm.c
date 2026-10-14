/*
 *  TCC IR - interleaved word copy through one base: LDM/STM chunks
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* codegen: a word copy that reaches codegen interleaved -- `ldr r0,[b,#s+4k];
 * str r0,[b,#d+4k]` per word, the shape ra:load_sink leaves -- was 2 instructions
 * a word.  When both sides share one base register and no store lands on a word
 * a later load reads (d <= s or the ranges are apart), it is copied in LDM/STM
 * chunks through registers free across the run (the ROMFS FileHeader.load tail). */

#include <stdint.h>
#include <stdio.h>

struct ifile
{
  const void *vtable;
  void *ptr;
  uint32_t mf[4];
  int32_t *refcount;
  uint32_t pad;
};
struct reader
{
  uint64_t off;
  uint64_t data_off;
  struct ifile dev;
};
struct header
{
  struct reader rd;
  struct ifile dev;
  uint32_t tag;
};

static struct reader init_at(struct ifile dev, uint64_t off, uint64_t data_off)
{
  struct reader r;
  r.dev = dev;
  r.off = off;
  r.data_off = data_off;
  return r;
}

/* The FileHeader.load tail: dev (+48) copied down into rd.dev (+16). */
__attribute__((noinline)) void reader_tail(struct header *h, int32_t a1, uint64_t data_off)
{
  struct ifile df = h->dev;
  struct reader t = init_at(df, (uint64_t)(int64_t)a1, data_off);
  h->rd = t;
}

/* Values live across the copy must survive the registers it borrows. */
__attribute__((noinline)) uint32_t tail_keeps_live(struct header *h, uint32_t a, uint32_t b, uint32_t c)
{
  uint32_t x = a * 3u + h->tag, y = b ^ c, z = c + a;
  struct ifile df = h->dev;
  struct reader t = init_at(df, a, (uint64_t)b << 32 | c);
  h->rd = t;
  return x ^ (y << 1) ^ (z << 2) ^ h->tag;
}

/* Overlapping, destination below the source: forward copy order. */
__attribute__((noinline)) void shift_down(uint32_t *p)
{
  p[0] = p[2];
  p[1] = p[3];
  p[2] = p[4];
  p[3] = p[5];
  p[4] = p[6];
  p[5] = p[7];
}

/* Overlapping, destination above the source: each later word reads one the
 * copy just wrote, so it may NOT be loaded as a block first. */
__attribute__((noinline)) void shift_up(uint32_t *p)
{
  p[2] = p[0];
  p[3] = p[1];
  p[4] = p[2];
  p[5] = p[3];
  p[6] = p[4];
}

/* Two pointers: they may alias, so the words stay one at a time. */
__attribute__((noinline)) void copy5(uint32_t *d, const uint32_t *s)
{
  d[0] = s[0];
  d[1] = s[1];
  d[2] = s[2];
  d[3] = s[3];
  d[4] = s[4];
}

static void dump(const char *tag, const uint32_t *w, int n)
{
  printf("%s", tag);
  for (int i = 0; i < n; i++)
    printf(" %x", (unsigned)w[i]);
  printf("\n");
}

int main(void)
{
  static int32_t rc = 7;
  struct header h;
  uint32_t *hw = (uint32_t *)&h;
  for (unsigned i = 0; i < sizeof h / 4; i++)
    hw[i] = 0xa0000000u + i;
  h.dev.vtable = (const void *)0x1111;
  h.dev.ptr = (void *)0x2222;
  h.dev.mf[0] = 0x30;
  h.dev.mf[1] = 0x31;
  h.dev.mf[2] = 0x32;
  h.dev.mf[3] = 0x33;
  h.dev.refcount = &rc;
  h.dev.pad = 0x44;
  reader_tail(&h, -48, 0x123456789abcdef0ull);
  dump("tail", hw, 8);
  printf("tail dev %d %d\n", h.rd.dev.refcount == &rc, h.dev.refcount == &rc);

  for (unsigned i = 0; i < sizeof h / 4; i++)
    hw[i] = 0xb0000000u + i;
  h.tag = 5;
  uint32_t r = tail_keeps_live(&h, 0x100, 0x2020, 0x30303);
  printf("live %x\n", (unsigned)r);
  dump("live", hw, 12);

  uint32_t a[10];
  for (int i = 0; i < 10; i++)
    a[i] = 0x100u + (unsigned)i;
  shift_down(a);
  dump("down", a, 10);
  for (int i = 0; i < 10; i++)
    a[i] = 0x200u + (unsigned)i;
  shift_up(a);
  dump("up", a, 10);
  for (int i = 0; i < 10; i++)
    a[i] = 0x300u + (unsigned)i;
  copy5(a + 1, a);
  dump("alias+", a, 10);
  for (int i = 0; i < 10; i++)
    a[i] = 0x400u + (unsigned)i;
  copy5(a, a + 2);
  dump("alias-", a, 10);
  return 0;
}
