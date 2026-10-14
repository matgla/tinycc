/*
 *  TCC Tests - Bounded read-only wrappers around a string scan
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

typedef unsigned u32;
typedef unsigned char u8;
struct slice { const u8 *ptr; u32 len; };

static u32 scan(const u8 *s)
{
  u32 n = 0;
  while (s[n])
    n++;
  return n;
}

static struct slice name_of(const u8 *const a0)
{
  const u8 *t0 = a0 + 4;
  struct slice t1;
  t1.ptr = t0;
  t1.len = scan(t0);
  return t1;
}

static u32 align_forward(u32 n, u32 a)
{
  if (a == 0 || (a & (a - 1u)) != 0)
    __builtin_unreachable();
  u32 m = a - 1u;
  return (n + m) & ~m;
}

static u32 record_size(const u8 *const a0, const u8 a1)
{
  const u8 *t0 = a0;
  const u8 *const *t1 = &t0;
  struct slice t2 = name_of(*t1);
  struct slice t3 = t2;
  const struct slice *t4 = &t3;
  const u32 *t5 = &t4->len;
  u32 t6 = *t5;
  u32 t7 = 4u + t6;
  t7 = t7 + 1u;
  t7 += (t6 & 3u) << 1;
  t7 ^= t6 >> 3;
  t7 += (t6 * 7u) & 15u;
  return align_forward(t7, (u32)a1);
}

u32 one_size(const u8 *p, u8 a)
{
  return record_size(p, a);
}

u32 two_sizes(const u8 *p, const u8 *q, u8 a)
{
  return record_size(p, a) + record_size(q, a);
}

u32 three_sizes(const u8 *p, const u8 *q, const u8 *r, u8 a)
{
  return record_size(p, a) + record_size(q, a) + record_size(r, a);
}

static volatile u32 observations;

__attribute__((noinline)) static u32 observed_scan(const u8 *s)
{
  observations++;
  return scan(s);
}

static u32 observed_size(const u8 *p, u8 a)
{
  const u8 *t0 = p;
  const u8 *const *t1 = &t0;
  u32 t2 = observed_scan(*t1 + 4);
  u32 t3 = t2;
  const u32 *t4 = &t3;
  u32 t6 = *t4;
  u32 t7 = 4u + t6;
  t7 = t7 + 1u;
  t7 += (t6 & 3u) << 1;
  t7 ^= t6 >> 3;
  t7 += (t6 * 7u) & 15u;
  return align_forward(t7, (u32)a);
}

u32 observed_one(const u8 *p, u8 a)
{
  return observed_size(p, a);
}

u32 observed_two(const u8 *p, u8 a)
{
  return observed_size(p, a) + observed_size(p + 1, a);
}

#define NAME_CALLER(N) u32 name_caller_##N(const u8 *p) { return name_of(p).len + N; }
NAME_CALLER(0)
NAME_CALLER(1)
NAME_CALLER(2)
NAME_CALLER(3)
NAME_CALLER(4)
NAME_CALLER(5)
NAME_CALLER(6)
NAME_CALLER(7)
NAME_CALLER(8)
NAME_CALLER(9)

u32 three_names(const u8 *a, const u8 *b, const u8 *c)
{
  return name_of(a).len + name_of(b).len + name_of(c).len;
}

static const u8 *next_record(const u8 *p, u8 a)
{
  return p + record_size(p, a);
}

u32 walk_records(const u8 *p, u32 count, u8 a)
{
  u32 bytes = 0;
  for (u32 i = 0; i < count; ++i) {
    bytes += record_size(p, a);
    p = next_record(p, a);
  }
  return bytes;
}

__attribute__((always_inline)) static inline u32 forced_scan(const u8 *p)
{
  u32 n = 0;
again:
  if (!p[n])
    return n;
  ++n;
  goto again;
}

u32 forced_name(const u8 *p)
{
  return forced_scan(p + 4);
}

static u32 sum_to(u32 count)
{
  u32 sum = 0;
  while (count)
    sum += count--;
  return sum;
}

u32 sum_name(u32 count)
{
  return sum_to(count);
}
