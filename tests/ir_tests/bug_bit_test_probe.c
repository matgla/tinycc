/*
 *  TCC IR - single-bit tests, unsigned zero tests and branch-to-branch probes
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* The page-pool free-run probe: `(x & (1 << n)) != 0` rematerialised the 1 on
 * every probe (now `(x >> n) & 1`), unsigned `x > 0` was never a CBZ/CBNZ, and
 * a branch to a lone `b` (or over one) survived codegen.  The neighbours pin
 * what must not change: a mask read as a value, a signed test of bit 31, a
 * mask with a second reader. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct Dyn { uint32_t bit_length; uint32_t *masks; };
struct BM { void *ptr; const void *vtable; struct Dyn u; };
struct Region { uint32_t start, page_count, page_size; struct BM bm; uint32_t hint; };
struct Opt { uint32_t payload; uint8_t is_null; };

static int is_set(struct Dyn d, uint32_t i)
{
  return (d.masks[i >> 5] & (1u << (i & 31))) != 0;
}

__attribute__((noinline)) uint32_t probe(const uint32_t *m, uint32_t start, uint32_t off)
{
  while (off > 0) {
    off--;
    uint32_t i = start + off;
    if ((m[i >> 5] & (1u << (i & 31))) != 0)
      return i;
  }
  return (uint32_t)-1;
}

__attribute__((noinline)) int bt_ne(uint32_t x, uint32_t n) { return (x & (1u << n)) != 0; }
__attribute__((noinline)) int bt_eq(uint32_t x, uint32_t n) { return (x & (1u << n)) == 0; }
__attribute__((noinline)) int bt_if(uint32_t x, uint32_t n) { if (x & (1u << n)) return 7; return 9; }
__attribute__((noinline)) uint32_t bt_sel(uint32_t x, uint32_t n, uint32_t a, uint32_t b)
{
  return (x & (1u << n)) ? a : b;
}
/* The masked value itself is the result: no 0/1 shortcut. */
__attribute__((noinline)) uint32_t bt_value(uint32_t x, uint32_t n) { return x & (1u << n); }
/* Signed test of the masked word: bit 31 is negative. */
__attribute__((noinline)) int bt_signed(uint32_t x, uint32_t n) { return (int32_t)(x & (1u << n)) > 0; }
/* The mask has a second reader. */
__attribute__((noinline)) int bt_mask_kept(uint32_t x, uint32_t n, uint32_t *out)
{
  uint32_t mask = 1u << n;
  *out = mask;
  return (x & mask) != 0;
}
/* The mask is built before the word is loaded. */
__attribute__((noinline)) int bt_mask_first(const uint32_t *p, uint32_t n) { return ((1u << n) & *p) != 0; }
/* Zig's `!(mask & bit != 0)` through reused locals: the negated 0/1 test is
 * the test itself with the branch swapped, no `eor #1`. */
static volatile uint32_t sched_mask = 0x5;
static volatile uint8_t pm_ready = 1;
__attribute__((noinline)) uint8_t core_id(uint32_t c) { return (uint8_t)c; }
__attribute__((noinline)) bool scheduled_here(uint32_t core)
{
  uint32_t t1, t2, t7, t3;
  uint8_t t4;
  bool t0, t8;
  t2 = sched_mask;
  t1 = t2;
  goto b2;
b2:;
  t4 = core_id(core);
  t3 = t4;
  goto b3;
b3:;
  t7 = (uint32_t)1 << (t3 & 31);
  t2 = t7;
  goto b4;
b4:;
  t2 = t1 & t2;
  t8 = t2 != 0;
  t0 = t8;
  goto b1;
b1:;
  t0 = !t0;
  if (t0)
    return false;
  return pm_ready != 0;
}

/* Unsigned zero tests, forward and in a loop. */
__attribute__((noinline)) uint32_t ugt0(uint32_t x, uint32_t a) { if (x > 0) a += 5; return a * 3; }
__attribute__((noinline)) uint32_t ule0(uint32_t x, uint32_t a) { if (x <= 0) a += 5; return a * 3; }
__attribute__((noinline)) uint32_t count_down(uint32_t n)
{
  uint32_t s = 0;
  while (n > 0) {
    s += n;
    n--;
  }
  return s;
}

/* ProcessMemoryPool.find_free_run: the probe nested in the window loop. */
__attribute__((noinline)) struct Opt find_free_run(const struct Region *r, uint32_t pages, uint32_t from,
                                                   uint32_t limit)
{
  struct Opt none = { 0xaaaaaaaau, 1 };
  if (pages == 0)
    return none;
  uint32_t start = from;
  while (start + pages <= limit) {
    uint32_t off = pages;
    struct Opt blocked = none;
    while (off > 0) {
      off--;
      if (is_set(r->bm.u, start + off)) {
        blocked.is_null = 0;
        blocked.payload = start + off;
        break;
      }
    }
    if (blocked.is_null)
      return (struct Opt){ start, 0 };
    start = blocked.payload + 1;
  }
  return none;
}

/* reserve_pages' region loop around it, with the hint scan ahead. */
__attribute__((noinline)) uint32_t probe_regions(struct Region *regions, uint32_t nregions, uint32_t pages)
{
  for (uint32_t i = 0; i < nregions; i++) {
    struct Region *r = &regions[i];
    uint32_t hint = r->hint;
    while (hint < r->page_count && is_set(r->bm.u, hint))
      hint++;
    r->hint = hint;
    struct Opt s = find_free_run(r, pages, hint, r->page_count);
    if (!s.is_null)
      return s.payload;
  }
  return (uint32_t)-1;
}

/* `b<c> L; b X; L:` once the phi copies coalesce: one inverted branch. */
__attribute__((noinline)) int valid_stack_ptr(uint32_t a0)
{
  uint32_t t0, t3;
  t0 = a0 % 4u;
  if (t0 != 0)
    return 0;
#if defined(__arm__)
  __asm volatile("mov %0, sp" : "=r"(t3));
#else
  t3 = (uint32_t)(uintptr_t)__builtin_frame_address(0) - 64;
#endif
  t0 = t3;
  if (a0 < t0)
    return 0;
  t0 = a0 - t0;
  return t0 < 8192;
}

__attribute__((noinline)) void mark(struct Region *r, uint32_t s, uint32_t e) { r->hint = s + e; }

/* The window loop's exit lands on a lone `b`: branch to its target. */
__attribute__((noinline)) uint32_t reserve(struct Region *r, uint32_t pages)
{
  if (pages == 0)
    return (uint32_t)-1;
  uint32_t start = r->hint;
  while (start + pages <= r->page_count) {
    struct Opt b = { 0xaaaaaaaau, 1 };
    uint32_t off = pages;
    while (off > 0) {
      off--;
      if (is_set(r->bm.u, start + off)) {
        b.payload = start + off;
        b.is_null = 0;
        break;
      }
    }
    if (b.is_null) {
      mark(r, start, start + pages);
      mark(r, start + 1, pages);
      return start;
    }
    start = b.payload + 1;
  }
  return (uint32_t)-1;
}

static uint32_t words_a[4] = { 0xffffffffu, 0x0000ffffu, 0x80000001u, 0x00000000u };
static uint32_t words_b[4] = { 0xfffffffeu, 0xffffffffu, 0xffffffffu, 0x7fffffffu };

int main(void)
{
  uint32_t kept = 0;
  printf("probe %u %u %u %u\n", probe(words_a, 0, 64), probe(words_a, 64, 64), probe(words_a, 96, 32),
         probe(words_a, 5, 0));
  printf("bt_ne %d %d %d %d\n", bt_ne(0x80000000u, 31), bt_ne(0x7fffffffu, 31), bt_ne(4, 2), bt_ne(4, 3));
  printf("bt_eq %d %d %d %d\n", bt_eq(0x80000000u, 31), bt_eq(0x7fffffffu, 31), bt_eq(4, 2), bt_eq(4, 3));
  printf("bt_if %d %d\n", bt_if(1u << 17, 17), bt_if(1u << 17, 16));
  printf("bt_sel %u %u\n", bt_sel(0x80000000u, 31, 11, 22), bt_sel(0x40000000u, 31, 11, 22));
  printf("bt_value %u %u %u\n", bt_value(0xffffffffu, 31), bt_value(0xffffffffu, 3), bt_value(0xfffffff7u, 3));
  printf("bt_signed %d %d %d\n", bt_signed(0x80000000u, 31), bt_signed(0x40000000u, 30), bt_signed(0, 30));
  int bk = bt_mask_kept(0x100u, 8, &kept);
  printf("bt_mask_kept %d %u\n", bk, kept);
  printf("bt_mask_first %d %d\n", bt_mask_first(&words_a[2], 31), bt_mask_first(&words_a[2], 30));
  printf("scheduled_here %d %d %d %d\n", scheduled_here(0), scheduled_here(1), scheduled_here(2), scheduled_here(34));
  printf("ugt0 %u %u ule0 %u %u\n", ugt0(0, 1), ugt0(0x80000000u, 1), ule0(0, 1), ule0(0x80000000u, 1));
  printf("count_down %u %u\n", count_down(0), count_down(100));

  uint32_t bits_a[4] = { 0x0000ff0fu, 0, 0x00f00000u, 0 };
  uint32_t bits_b[2] = { 0xffffffffu, 0xffffffffu };
  struct Region regs[2] = {
    { 0, 64, 4096, { 0, 0, { 64, bits_b } }, 0 },
    { 0, 128, 4096, { 0, 0, { 128, bits_a } }, 0 },
  };
  struct Opt o1 = find_free_run(&regs[1], 4, 0, 128);
  struct Opt o2 = find_free_run(&regs[1], 40, 0, 128);
  struct Opt o3 = find_free_run(&regs[1], 200, 0, 128);
  struct Opt o4 = find_free_run(&regs[1], 0, 0, 128);
  printf("find %u/%d %u/%d %u/%d %u/%d\n", o1.payload, o1.is_null, o2.payload, o2.is_null, o3.payload, o3.is_null,
         o4.payload, o4.is_null);
  uint32_t pr = probe_regions(regs, 2, 3);
  printf("regions %u %u %u\n", pr, regs[0].hint, regs[1].hint);
  printf("regions %u\n", probe_regions(regs, 2, 128));
  printf("regions %u\n", probe_regions(regs, 1, 1));
  uint32_t local = 0;
  printf("valid_stack_ptr %d %d %d %d\n", valid_stack_ptr((uint32_t)(uintptr_t)&local),
         valid_stack_ptr((uint32_t)(uintptr_t)&local + 1), valid_stack_ptr(0),
         valid_stack_ptr((uint32_t)(uintptr_t)&local + 0x10000));
  regs[1].hint = 0;
  uint32_t r1 = reserve(&regs[1], 4);
  uint32_t h1 = regs[1].hint;
  uint32_t r2 = reserve(&regs[1], 40);
  uint32_t h2 = regs[1].hint;
  uint32_t r3 = reserve(&regs[1], 200);
  uint32_t r4 = reserve(&regs[1], 0);
  printf("reserve %u %u %u %u %u %u\n", r1, h1, r2, h2, r3, r4);
  (void)words_b;
  return 0;
}
