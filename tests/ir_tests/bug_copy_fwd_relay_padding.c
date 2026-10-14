/*
 *  TCC IR - copies of an optional struct between locals
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ssa:copy_fwd, the Zig C backend's ?FileHeader walk: `t16 = t9` only to test
 * t16.is_null kept its 160-byte copy, because `t16.is_null = false;
 * t16.payload = t21` left t16's padding as the copy's; and that relay copied
 * on with `t9 = t16`.  Member stores now stand for a whole new value of t16
 * (its padding is unspecified), and stores into a relay that dies at its copy
 * write the destination instead.  The other functions pin the shapes where a
 * copy must stay (a vector, a VT_STRUCT without members, has no padding). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Hdr
{
  uint64_t id;
  uint32_t w[34];
  uint8_t ft;
  uint8_t len;
}; /* 152 bytes, 6 of them padding */
struct OptHdr
{
  struct Hdr payload;
  bool is_null;
}; /* 160 bytes */
struct EHdr
{
  struct Hdr payload;
  uint16_t error;
};

static unsigned sum(const struct Hdr *h)
{
  unsigned s = (unsigned)h->id ^ (unsigned)(h->id >> 32);
  for (int i = 0; i < 34; i++)
    s = s * 31 + h->w[i];
  return s * 7 + h->ft * 3 + h->len;
}

static void fill(struct Hdr *h, int k)
{
  h->id = 0x100000000ull * (unsigned)k + 11u;
  for (int i = 0; i < 34; i++)
    h->w[i] = (uint32_t)(k * 100 + i);
  h->ft = (uint8_t)(k & 3);
  h->len = (uint8_t)(k + 1);
}

__attribute__((noinline)) static struct OptHdr make_opt(int k)
{
  struct OptHdr o;
  memset(&o, 0, sizeof o);
  if (k < 0)
  {
    o.is_null = true;
    return o;
  }
  fill(&o.payload, k);
  return o;
}

static int visits;
static const struct Hdr *watched;
static unsigned watched_sum;

/* reads (and bumps) the header its caller hands out */
__attribute__((noinline)) static unsigned visit(struct Hdr *h)
{
  visits++;
  h->w[5] += 1;
  return h->w[0] + h->ft;
}

/* looks at whatever `watched` points to */
__attribute__((noinline)) static void observe(void)
{
  watched_sum = watched ? sum(watched) : 0;
}

__attribute__((noinline)) static struct EHdr make_hdr(int k)
{
  struct EHdr e;
  memset(&e, 0, sizeof e);
  if (k == 7)
  {
    e.error = 3;
    return e;
  }
  fill(&e.payload, k + 20);
  return e;
}

/* walk's step loop, spelled as the Zig C backend spells it */
__attribute__((noinline)) static struct EHdr walk(int n, unsigned stop_at)
{
  struct OptHdr t9, t16;
  struct EHdr t15, t22;
  struct Hdr t21;
  t16 = make_opt(n);
  t9 = t16;
  for (int i = 0;; i++)
  {
    t16 = t9;
    if (t16.is_null)
      break;
    if (visit(&t9.payload) > stop_at)
    {
      t16 = t9;
      t21 = t16.payload;
      t15.payload = t21;
      t15.error = 0;
      t22 = t15;
      return t22;
    }
    if (i == n)
    {
      t9 = (struct OptHdr){.is_null = true};
      continue;
    }
    t22 = make_hdr(i);
    if (t22.error)
      return t22;
    t21 = t22.payload;
    t16.is_null = false;
    t16.payload = t21;
    t9 = t16;
  }
  t15.error = 9;
  memset(&t15.payload, 0, sizeof t15.payload);
  return t15;
}

/* only w[0] is stored: is_null and the rest of the payload are the copy's */
__attribute__((noinline)) static struct OptHdr partial(int k)
{
  struct OptHdr t9, t16;
  t9 = make_opt(k);
  visit(&t9.payload);
  t16 = t9;
  t16.payload.w[0] = 5;
  t9 = t16;
  return t9;
}

/* t16 is read after it is copied: its stores stay its own */
__attribute__((noinline)) static unsigned read_after(int k, struct OptHdr *out)
{
  struct OptHdr t9, t16;
  struct EHdr t22;
  t9 = make_opt(-1);
  visit(&t9.payload);
  t22 = make_hdr(k);
  t16.is_null = false;
  t16.payload = t22.payload;
  t9 = t16;
  *out = t9;
  return t16.payload.w[3] + sum(&t16.payload);
}

/* t9 is read between the stores into t16 and the copy */
__attribute__((noinline)) static unsigned read_between(int k, struct OptHdr *out)
{
  struct OptHdr t9, t16;
  struct EHdr t22;
  t9 = make_opt(k);
  t22 = make_hdr(k + 1);
  t16.payload = t22.payload;
  unsigned old = t9.payload.w[2];
  t16.is_null = false;
  t9 = t16;
  *out = t9;
  return old;
}

/* t9's address is out: a call between the stores and the copy may read it */
__attribute__((noinline)) static unsigned call_between(int k, struct OptHdr *out)
{
  struct OptHdr t9, t16;
  struct EHdr t22;
  t9 = make_opt(k);
  watched = &t9.payload;
  t22 = make_hdr(k + 2);
  t16.payload = t22.payload;
  t16.is_null = false;
  observe();
  t9 = t16;
  watched = NULL;
  *out = t9;
  return watched_sum;
}

/* a vector has no members, and no padding: v[15] alone is no new value of v */
typedef unsigned char V16 __attribute__((vector_size(16)));

__attribute__((noinline)) static void vset(V16 *p)
{
  V16 v = *p;
  v[15] = 1;
  *p = v;
}

static void show(const char *tag, const struct EHdr *e)
{
  printf("%s err=%u id=%llx w0=%u w5=%u ft=%u len=%u sum=%u\n", tag, e->error, (unsigned long long)e->payload.id,
         e->payload.w[0], e->payload.w[5], e->payload.ft, e->payload.len, sum(&e->payload));
}

static void show_opt(const char *tag, const struct OptHdr *o, unsigned extra)
{
  printf("%s null=%d id=%llx w0=%u w5=%u sum=%u extra=%u\n", tag, o->is_null, (unsigned long long)o->payload.id,
         o->payload.w[0], o->payload.w[5], sum(&o->payload), extra);
}

int main(void)
{
  struct EHdr r = walk(3, 1000000u);
  show("walk done", &r);
  r = walk(9, 1000000u);
  show("walk error", &r);
  r = walk(5, 2300u);
  show("walk found", &r);
  r = walk(-1, 0);
  show("walk empty", &r);
  printf("visits=%d\n", visits);

  struct OptHdr o = partial(4);
  show_opt("partial", &o, 0);
  unsigned x = read_after(2, &o);
  show_opt("read_after", &o, x);
  x = read_between(3, &o);
  show_opt("read_between", &o, x);
  x = call_between(6, &o);
  show_opt("call_between", &o, x);

  V16 v = {0};
  for (int i = 0; i < 15; i++)
    v[i] = (unsigned char)(i + 2);
  vset(&v);
  for (int i = 0; i < 16; i++)
    printf("%d%c", v[i], i == 15 ? '\n' : ' ');
  return 0;
}
