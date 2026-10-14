/*
 *  TCC IR - internal functions return 8-byte aggregates in r0/r1
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A static function's direct calls take an 8-byte, word-aligned aggregate in
 * r0/r1 instead of through an sret buffer (arm-thumb-emit.c
 * gfunc_regpair_ret); the body builds it member by member (ret.c), the caller
 * stores it the same way (call.c).  An address taken after a direct call or
 * the definition names an AAPCS adapter `f.aapcs`; one taken first keeps the
 * function AAPCS.  Every shape below must agree on both sides. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct opt { uintptr_t payload; bool is_null; };
struct eu { void *payload; uint16_t error; };
struct sl { const char *ptr; uintptr_t len; };
struct mix { uint8_t a; uint8_t b; uint16_t c; int32_t d; };
struct ll { long long v; };
struct fi { float f; int32_t i; };
struct dbl { double d; };
union un { uint64_t v; uint32_t w[2]; };
struct bf { uint32_t a : 3; uint32_t b : 29; int32_t c; };
struct nest { struct { uint16_t x; uint8_t y; } in; int32_t z; };
struct arr { int16_t v[4]; };
struct q4 { uint16_t a, b, c, d; };

static const uintptr_t vals[8] = {0, 3, 0, 0, 7, 9, 0, 1};
static char heap[64];

__attribute__((noinline)) static struct opt find(uintptr_t from, uintptr_t want)
{
  for (uintptr_t i = from; i < 8; i++)
    if (vals[i] == want)
      return (struct opt){i, false};
  return (struct opt){0xaaaaaaaa, true};
}

__attribute__((noinline)) static struct eu create(uintptr_t n)
{
  struct eu r;
  if (n > sizeof(heap))
    return (struct eu){.payload = (void *)(uintptr_t)0xaaaaaaaa, .error = 8};
  r.payload = heap + n;
  r.error = 0;
  return r;
}

__attribute__((noinline)) static struct sl trim(const char *p, uintptr_t n)
{
  while (n && *p == ' ')
  {
    p++;
    n--;
  }
  while (n && p[n - 1] == ' ')
    n--;
  return (struct sl){p, n};
}

/* Three arguments: the adapter moves r1-r3 down to r0-r2. */
__attribute__((noinline)) static struct mix mk_mix(int a, int b, int c)
{
  struct mix m;
  m.a = (uint8_t)a;
  m.b = (uint8_t)(a + b);
  m.c = (uint16_t)(b * 1000 + c);
  m.d = -c;
  return m;
}

/* A long long argument: r2:r3 under sret, r0:r1 here. */
__attribute__((noinline)) static struct ll mk_ll(long long x)
{
  struct ll r = {x * 3 - 1};
  return r;
}

__attribute__((noinline)) static struct fi mk_fi(float f, int i)
{
  return (struct fi){f * 2.0f, i + 1};
}

__attribute__((noinline)) static struct dbl mk_dbl(double d)
{
  return (struct dbl){d / 4};
}

/* Not read member by member: a union, a bit-field. */
__attribute__((noinline)) static union un mk_un(uint32_t lo, uint32_t hi)
{
  union un u;
  u.w[0] = lo;
  u.w[1] = hi;
  return u;
}

__attribute__((noinline)) static struct bf mk_bf(uint32_t a, uint32_t b)
{
  struct bf r;
  r.a = a;
  r.b = b;
  r.c = (int32_t)(a - b);
  return r;
}

__attribute__((noinline)) static struct nest mk_nest(int x)
{
  struct nest n;
  n.in.x = (uint16_t)(x * 7);
  n.in.y = (uint8_t)x;
  n.z = x - 100;
  return n;
}

__attribute__((noinline)) static struct arr mk_arr(int16_t base)
{
  struct arr a;
  for (int i = 0; i < 4; i++)
    a.v[i] = (int16_t)(base - i);
  return a;
}

/* Too many argument words for an adapter: it stays AAPCS throughout. */
__attribute__((noinline)) static struct q4 mk_q4(uint16_t a, long long b, uint16_t c)
{
  return (struct q4){a, (uint16_t)b, (uint16_t)(b >> 16), c};
}

/* By-value aggregate argument, recursion, a call's result returned. */
__attribute__((noinline)) static struct sl shrink(struct sl s, int k)
{
  if (k == 0 || s.len == 0)
    return s;
  return shrink((struct sl){s.ptr + 1, s.len - 1}, k - 1);
}

__attribute__((noinline)) static struct sl via_ptr(const struct sl *p)
{
  return *p;
}

/* Inlinable: no attribute. */
static struct opt first_set(uintptr_t from)
{
  for (uintptr_t i = from; i < 8; i++)
    if (vals[i])
      return (struct opt){i, false};
  return (struct opt){0, true};
}

/* Address taken first: these stay AAPCS. */
static struct sl first_word(const char *p, uintptr_t n);
static struct sl (*const early_table[1])(const char *, uintptr_t) = {first_word};
static struct sl first_word(const char *p, uintptr_t n)
{
  uintptr_t i = 0;
  while (i < n && p[i] != ' ')
    i++;
  return (struct sl){p, i};
}

static void print_sl(const char *tag, struct sl s)
{
  printf("%s [%.*s] %lu\n", tag, (int)s.len, s.ptr, (unsigned long)s.len);
}

int main(void)
{
  struct opt o = find(0, 7);
  struct opt o2 = find(5, 7);
  printf("find %lu %d %lu %d\n", (unsigned long)o.payload, o.is_null, (unsigned long)o2.payload, o2.is_null);

  unsigned n = 0;
  for (struct opt b = first_set(0); !b.is_null; b = first_set(b.payload + 1))
    n++;
  printf("first_set %u\n", n);

  struct eu e1 = create(10), e2 = create(100);
  printf("create %ld %u %u\n", (long)((char *)e1.payload - heap), e1.error, e2.error);

  print_sl("trim", trim("  hello world  ", 15));
  printf("trim.len %lu\n", (unsigned long)trim("   x", 4).len);

  struct mix m = mk_mix(3, 4, 5);
  printf("mix %u %u %u %ld\n", m.a, m.b, m.c, (long)m.d);
  printf("ll %lld\n", mk_ll(0x100000001LL).v);
  struct fi f = mk_fi(1.25f, 41);
  printf("fi %d %ld\n", (int)(f.f * 100), (long)f.i);
  printf("dbl %d\n", (int)(mk_dbl(10.0).d * 100));
  union un u = mk_un(0x11223344u, 0x55667788u);
  printf("un %08lx %08lx\n", (unsigned long)(u.v >> 32), (unsigned long)(uint32_t)u.v);
  struct bf b = mk_bf(5, 1000);
  printf("bf %u %u %ld\n", (unsigned)b.a, (unsigned)b.b, (long)b.c);
  struct nest ne = mk_nest(9);
  printf("nest %u %u %ld\n", ne.in.x, ne.in.y, (long)ne.z);
  struct arr ar = mk_arr(20);
  printf("arr %d %d %d %d\n", ar.v[0], ar.v[1], ar.v[2], ar.v[3]);
  struct q4 q = mk_q4(1, 0x30002LL, 4);
  printf("q4 %u %u %u %u\n", q.a, q.b, q.c, q.d);

  struct sl s = {"abcdef", 6};
  print_sl("shrink", shrink(s, 2));
  print_sl("via_ptr", via_ptr(&s));

  /* Addresses taken after the direct calls: the adapters. */
  struct mix (*pm)(int, int, int) = mk_mix;
  struct sl (*pt)(const char *, uintptr_t) = trim;
  struct opt (*const table[2])(uintptr_t, uintptr_t) = {find, find};
  struct mix m2 = pm(7, 8, 9);
  printf("pm %u %u %u %ld\n", m2.a, m2.b, m2.c, (long)m2.d);
  print_sl("pt", pt(" ab ", 4));
  struct opt o3 = table[1](0, 9);
  printf("table %lu %d\n", (unsigned long)o3.payload, o3.is_null);
  struct ll (*pll)(long long) = mk_ll;
  printf("pll %lld\n", pll(-5));
  struct sl (*psh)(struct sl, int) = shrink;
  print_sl("psh", psh(s, 3));
  printf("same %d %d\n", (void *)pm == (void *)mk_mix, (void *)table[0] == (void *)find);

  /* The address taken before any call. */
  print_sl("early", early_table[0]("one two", 7));
  print_sl("early direct", first_word("three four", 10));

  /* A result nobody reads, results as arguments. */
  find(1, 3);
  printf("args %lu %lu\n", (unsigned long)find(0, 9).payload, (unsigned long)trim(" a ", 3).len);
  return 0;
}
