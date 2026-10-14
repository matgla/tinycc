/*
 *  TCC IR - ROMFS FileHeader.load tail: u64 fields of the reader temporary
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* sra: a 64-bit store through an address TEMP (the Zig C backend's
 * `t2 = &t0._offset; *t2 = a1;`) kept the whole reader temporary in memory;
 * it is now a split doubleword like a direct 64-bit slot access.
 * ssa:fold: the low word of `P PACK64 x` is P when the parameter is never
 * written.  ssa:loop_header_dup: copies and constants in a header do not count
 * against its size cap (the inlined zig_u32_bitCast_u32 bindings).
 * branch_tidy: identity moves between a B<c> and a B do not block inverting
 * the pair.  ra:load_sink: a copied word's load moves down to its store. */

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
struct file_reader
{
  uint64_t offset;
  uint64_t data_offset;
  struct ifile device;
};
struct file_header
{
  struct file_reader reader;
  struct ifile device;
  uint32_t name_len;
};

/* fs.romfs.file_reader.FileReader.init_at, as zig emits it. */
static struct file_reader init_at(struct ifile const a0, uint64_t const a1, uint64_t const a2)
{
  struct file_reader t0;
  struct ifile *t1;
  uint64_t *t2;
  t1 = &t0.device;
  *t1 = a0;
  t2 = &t0.offset;
  *t2 = a1;
  t2 = &t0.data_offset;
  *t2 = a2;
  return t0;
}

/* The same, with the fields named directly. */
static struct file_reader init_direct(struct ifile const a0, uint64_t const a1, uint64_t const a2)
{
  struct file_reader t0;
  t0.device = a0;
  t0.offset = a1;
  t0.data_offset = a2;
  return t0;
}

__attribute__((noinline)) void zig_tail(struct file_header *self, int32_t off, uint64_t data_offset)
{
  struct file_reader *dst = &self->reader;
  struct ifile df = self->device;
  uint64_t o = (uint64_t)(int64_t)off;
  struct file_reader t64 = init_at(df, o, data_offset);
  *dst = t64;
}

__attribute__((noinline)) void direct_tail(struct file_header *self, int32_t off, uint64_t data_offset)
{
  struct ifile df = self->device;
  struct file_reader t64 = init_direct(df, (uint64_t)(int64_t)off, data_offset);
  self->reader = t64;
}

/* The u64 fields stored whole through the pointer, read back a word at a time. */
__attribute__((noinline)) uint32_t lo_after_store(int32_t off, uint64_t d)
{
  union
  {
    struct file_reader r;
    uint32_t w[12];
  } t0;
  uint64_t *t2 = &t0.r.offset;
  *t2 = (uint64_t)(int64_t)off;
  t2 = &t0.r.data_offset;
  *t2 = d;
  return t0.w[0] ^ t0.w[1] ^ (t0.w[2] + t0.w[3]);
}

/* A parameter written in the function: its later value is not the packed one. */
__attribute__((noinline)) uint32_t written_param(int32_t off, int32_t step)
{
  uint64_t packed = (uint64_t)(int64_t)off;
  off += step;
  return (uint32_t)packed + (uint32_t)off;
}

/* ra:load_sink must keep a load above a store that writes its bytes, through
 * the same base or through another pointer that may alias it. */
__attribute__((noinline)) void copy_overlap(uint32_t *p)
{
  uint32_t a = p[0], b = p[1], c = p[4];
  p[1] = 7;
  p[5] = 9;
  p[2] = a;
  p[3] = b;
  p[6] = c;
}

__attribute__((noinline)) void copy_alias(uint32_t *p, uint32_t *q)
{
  uint32_t a = p[0], b = p[1];
  q[1] = 0x55;
  p[2] = a;
  p[3] = b;
}

/* A parameter's words copied into a frame array (Parser.validate): the loads
 * sink past the other words' frame stores. */
__attribute__((noinline)) uint32_t frame_copy(const uint32_t *p, uint32_t lim)
{
  uint32_t a[6];
  uint32_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], a4 = p[4], a5 = p[5];
  a[0] = a0;
  a[1] = a1;
  a[2] = a2;
  a[3] = a3;
  a[4] = a4;
  a[5] = a5;
  uint32_t n = 0;
  for (uint32_t i = 0; i < 6; i++)
    n += a[i] < lim ? a[i] : i;
  return n;
}

/* std.mem.findScalarPos with zig.h's bitCast -> truncate -> shr helpers
 * inlined: the header is copies, two `#-1` masks and two ANDs. */
static inline uint32_t zig_shr_u32(uint32_t lhs, uint8_t rhs)
{
  return lhs >> rhs;
}
static inline uint32_t zig_u32_truncate_u32(uint32_t arg, uint8_t bits)
{
  return arg & zig_shr_u32(UINT32_MAX, 32 - bits);
}
static inline uint32_t zig_u32_bitCast_u32(uint32_t arg, uint8_t bits)
{
  return zig_u32_truncate_u32(arg, bits);
}

struct slice_u8
{
  const uint8_t *ptr;
  uintptr_t len;
};

__attribute__((noinline)) int find_zero(const struct slice_u8 *s, uintptr_t start, uintptr_t *last)
{
  struct slice_u8 t9 = *s;
  uintptr_t t8 = 0, t12, t11 = t9.len - start, seen = 0;
  for (;;)
  {
    t12 = t8;
    seen = t12;
    uint32_t t5 = zig_u32_bitCast_u32(t12, 32);
    uint32_t t4 = zig_u32_bitCast_u32(t11, 32);
    if (t5 < t4)
    {
      if (t9.ptr[start + t12] == 0)
        return (int)(start + t12);
      t12 = t12 + 1;
      t8 = t12;
      continue;
    }
    break;
  }
  *last = seen;
  return -1;
}

/* The found path is `idx <- i` into the same register (an identity move):
 * branch_tidy looks past it, so the latch is `cmp; bcc head`, not
 * `cmp; bcs exit; b head`. */
struct name_hdr
{
  uint32_t w[4];
  uint8_t name[16];
  uint8_t len;
};

__attribute__((noinline)) void name_len(struct name_hdr *h)
{
  const uint8_t *p = h->name;
  uintptr_t t8 = 0, t12, idx;
  for (;;)
  {
    t12 = t8;
    uint32_t t5 = zig_u32_bitCast_u32(t12, 32);
    uint32_t t4 = zig_u32_bitCast_u32(16, 32);
    if (t5 < t4)
    {
      if (p[t12] == 0)
      {
        idx = t12;
        goto found;
      }
      t12 = t12 + 1;
      t8 = t12;
      continue;
    }
    break;
  }
  h->w[0] = 0xAAAAAAAAu;
  return;
found:
  h->len = (uint8_t)idx;
}

static void dump(const char *tag, const struct file_header *h)
{
  printf("%s %08x %08x %08x %08x", tag, (unsigned)h->reader.offset, (unsigned)(h->reader.offset >> 32),
         (unsigned)h->reader.data_offset, (unsigned)(h->reader.data_offset >> 32));
  for (int i = 0; i < 4; i++)
    printf(" %x", (unsigned)h->reader.device.mf[i]);
  printf(" %x %x %x %x %u\n", (unsigned)(uintptr_t)h->reader.device.vtable, (unsigned)(uintptr_t)h->reader.device.ptr,
         (unsigned)(uintptr_t)h->reader.device.refcount, (unsigned)h->reader.device.pad, (unsigned)h->name_len);
}

int main(void)
{
  struct file_header h;
  __builtin_memset(&h, 0x5a, sizeof h);
  h.device.vtable = (const void *)0x1000;
  h.device.ptr = (void *)0x2000;
  for (int i = 0; i < 4; i++)
    h.device.mf[i] = 0x100u + (unsigned)i;
  h.device.refcount = (int32_t *)0x3000;
  h.device.pad = 0x77;
  h.name_len = 9;

  zig_tail(&h, 0x1234, 32);
  dump("zig+", &h);
  zig_tail(&h, -48, 0x0000000500000040ull);
  dump("zig-", &h);
  direct_tail(&h, 0x7fffffff, 0xfedcba9876543210ull);
  dump("dir+", &h);
  direct_tail(&h, (int32_t)0x80000000, 0);
  dump("dir-", &h);

  printf("lo %08x %08x\n", (unsigned)lo_after_store(-1, 0x100000002ull), (unsigned)lo_after_store(5, 7));
  printf("param %08x %08x\n", (unsigned)written_param(10, 3), (unsigned)written_param(-2, -5));

  uint32_t w[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  copy_overlap(w);
  printf("overlap %u %u %u %u %u %u %u\n", w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
  printf("frame %u %u\n", (unsigned)frame_copy(w, 6), (unsigned)frame_copy(w + 1, 100));
  uint32_t x[4] = {11, 12, 13, 14};
  copy_alias(x, x);
  printf("alias %u %u %u %u\n", x[0], x[1], x[2], x[3]);

  uint8_t buf[20];
  for (int i = 0; i < 20; i++)
    buf[i] = (uint8_t)(i + 1);
  struct slice_u8 sl = {buf, 20};
  uintptr_t last = 0;
  int r = find_zero(&sl, 4, &last);
  printf("scan none %d %u\n", r, (unsigned)last);
  for (int z = 19; z >= 0; z -= 6)
  {
    buf[z] = 0;
    last = 77;
    r = find_zero(&sl, 4, &last);
    printf("scan %d %d %u\n", z, r, (unsigned)last);
  }
  struct name_hdr nh;
  for (int z = 16; z >= 0; z -= 5)
  {
    __builtin_memset(&nh, 0x41, sizeof nh);
    if (z < 16)
      nh.name[z] = 0;
    name_len(&nh);
    printf("name %d %x %u\n", z, (unsigned)nh.w[0], (unsigned)nh.len);
  }
  sl.len = 4;
  last = 5;
  r = find_zero(&sl, 4, &last);
  printf("scan empty %d %u\n", r, (unsigned)last);
  return 0;
}
