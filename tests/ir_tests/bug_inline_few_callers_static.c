/*
 *  TCC IR - static helpers with a few call sites inline at -O2
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The kernel's bit_set setRangeValue (two sites, `value` constant at both)
 * stayed a call: too large for a small leaf.  A static leaf called from a few
 * sites now inlines within a growth bound (generators/function.c,
 * FEW_CALLERS_GROWTH_OPS).  TmpMemoryPool.next_bit returns ?usize through
 * memory and its ctz made pure_via_sret reject it, so the aggregate-return
 * loop class never took it: purity now asks op_props. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define ALL_ONES (~(uintptr_t)0)

struct range { uintptr_t start; uintptr_t end; };
struct bitset { uintptr_t bit_length; uintptr_t *masks; };
struct managed { void *alloc; const void *vtable; struct bitset bits; };
struct opt_usize { uintptr_t payload; bool is_null; };
struct pool { uintptr_t pages; struct managed map; uintptr_t used; uintptr_t hint; };

static uintptr_t bool_mask(bool v) { return (uintptr_t)0 - (uintptr_t)(uint8_t)v; }

/* bit_set.Dynamic.setRangeValue: called once, from the managed wrapper. */
static void dyn_set_range(struct bitset *const self, struct range const r, bool const value)
{
  if (r.start == r.end)
    return;
  uintptr_t const start_idx = r.start / 32;
  uint8_t const start_bit = (uint8_t)(r.start & 31);
  uintptr_t const end_idx = r.end / 32;
  uint8_t const end_bit = (uint8_t)(r.end & 31);
  if (start_idx == end_idx)
  {
    uintptr_t mask1 = ALL_ONES << start_bit;
    uintptr_t mask2 = ALL_ONES >> (uint8_t)(31 - (uint8_t)(end_bit - 1));
    self->masks[start_idx] &= ~(mask1 & mask2);
    mask1 = bool_mask(value) << start_bit;
    mask2 = bool_mask(value) >> (uint8_t)(31 - (uint8_t)(end_bit - 1));
    self->masks[start_idx] |= mask1 & mask2;
  }
  else
  {
    uintptr_t bulk = start_idx;
    if (start_bit > 0)
    {
      self->masks[start_idx] = (self->masks[start_idx] & ~(ALL_ONES << start_bit)) | (bool_mask(value) << start_bit);
      bulk = start_idx + 1;
    }
    for (; bulk < end_idx; bulk++)
      self->masks[bulk] = bool_mask(value);
    if (end_bit > 0)
      self->masks[end_idx] = (self->masks[end_idx] & (ALL_ONES << end_bit)) |
                             (bool_mask(value) >> (uint8_t)(31 - (uint8_t)(end_bit - 1)));
  }
}

/* bit_set.DynamicManaged.setRangeValue: two call sites, `value` constant at both. */
static void set_range_value(struct managed *const self, struct range const r, bool const value)
{
  dyn_set_range(&self->bits, r, value);
}

/* TmpMemoryPool.next_bit, want_set = true. */
static struct opt_usize next_set(struct pool *const self, uintptr_t const start, uintptr_t const end)
{
  if (start >= end)
    return (struct opt_usize){.is_null = true, .payload = 0xaaaaaaaau};
  uintptr_t *const masks = self->map.bits.masks;
  uintptr_t word = start / 32;
  uintptr_t bits = masks[word] & (ALL_ONES << (start % 32));
  for (;;)
  {
    if (bits != 0)
    {
      uintptr_t index = word * 32 + (uintptr_t)__builtin_ctz(bits);
      if (index < end)
        return (struct opt_usize){.is_null = false, .payload = index};
      return (struct opt_usize){.is_null = true, .payload = 0xaaaaaaaau};
    }
    word++;
    if (word * 32 >= end)
      return (struct opt_usize){.is_null = true, .payload = 0xaaaaaaaau};
    bits = masks[word];
  }
}

/* want_set = false. */
static struct opt_usize next_clear(struct pool *const self, uintptr_t const start, uintptr_t const end)
{
  if (start >= end)
    return (struct opt_usize){.is_null = true, .payload = 0xaaaaaaaau};
  uintptr_t *const masks = self->map.bits.masks;
  uintptr_t word = start / 32;
  uintptr_t bits = ~masks[word] & (ALL_ONES << (start % 32));
  for (;;)
  {
    if (bits != 0)
    {
      uintptr_t index = word * 32 + (uintptr_t)__builtin_ctz(bits);
      if (index < end)
        return (struct opt_usize){.is_null = false, .payload = index};
      return (struct opt_usize){.is_null = true, .payload = 0xaaaaaaaau};
    }
    word++;
    if (word * 32 >= end)
      return (struct opt_usize){.is_null = true, .payload = 0xaaaaaaaau};
    bits = ~masks[word];
  }
}

/* Called once: release_range expands it, next_set with it. */
static uintptr_t used_in_range(struct pool *self, uintptr_t start, uintptr_t end)
{
  uintptr_t used = 0;
  for (uintptr_t i = start;;)
  {
    struct opt_usize b = next_set(self, i, end);
    if (b.is_null)
      return used;
    used++;
    i = b.payload + 1;
  }
}

/* The same count for main, which must not add a site of used_in_range. */
uintptr_t used_between(struct pool *self, uintptr_t start, uintptr_t end)
{
  uintptr_t used = 0;
  for (struct opt_usize b = next_set(self, start, end); !b.is_null; b = next_set(self, b.payload + 1, end))
    used++;
  return used;
}

void claim_range(struct pool *self, uintptr_t start, uintptr_t end)
{
  set_range_value(&self->map, (struct range){start, end}, true);
  self->used += end - start;
  if (start == self->hint)
    self->hint = end;
}

void release_range(struct pool *self, uintptr_t start, uintptr_t end)
{
  if (start >= end)
    return;
  self->used -= used_in_range(self, start, end);
  set_range_value(&self->map, (struct range){start, end}, false);
  if (start < self->hint)
    self->hint = start;
}

/* First run of `n` clear bits at or after `from`; pages when there is none. */
uintptr_t find_free(struct pool *self, uintptr_t from, uintptr_t n)
{
  uintptr_t i = from;
  for (;;)
  {
    struct opt_usize c = next_clear(self, i, self->pages);
    if (c.is_null || c.payload + n > self->pages)
      return self->pages;
    struct opt_usize s = next_set(self, c.payload, c.payload + n);
    if (s.is_null)
      return c.payload;
    i = s.payload + 1;
  }
}

/* The next set bit in [from, to) or -1, and the next clear one. */
long first_set(struct pool *self, uintptr_t from, uintptr_t to)
{
  struct opt_usize s = next_set(self, from, to);
  return s.is_null ? -1 : (long)s.payload;
}

long first_clear(struct pool *self, uintptr_t from, uintptr_t to)
{
  struct opt_usize s = next_clear(self, from, to);
  return s.is_null ? -1 : (long)s.payload;
}

/* Returns through memory and scans with ctz, but counts its calls: not pure,
 * so a call whose result is unused still runs. */
static unsigned scans;
static struct opt_usize counted_scan(uintptr_t word, uintptr_t end)
{
  scans++;
  for (uintptr_t i = 0; i < 4; i++)
  {
    if (word)
    {
      uintptr_t index = i * 32 + (uintptr_t)__builtin_ctz(word);
      return (struct opt_usize){.is_null = index >= end, .payload = index};
    }
    word = word * 2 + 1;
  }
  return (struct opt_usize){.is_null = true, .payload = 0};
}

void scan_twice(uintptr_t word)
{
  counted_scan(word, 64);
  counted_scan(word >> 1, 3);
}

/* Two direct sites, but recursive: it must still compile and stay correct. */
static unsigned walk_down(unsigned x, unsigned acc)
{
  while (x > 3 && (x & 1))
    x = x * 3 + 1;
  if (x <= 1)
    return acc;
  return walk_down(x / 2, acc + 1);
}

unsigned walk_a(unsigned x) { return walk_down(x, 0); }
unsigned walk_b(unsigned x) { return walk_down(x + 7, 100); }

/* Two direct sites plus its address in a table: the calls through the
 * table must still reach a body with the same results. */
static int fold_steps(int a, int k)
{
  int r = 0;
  for (int i = 0; i < k; i++)
    r = r * 3 + (a ^ i);
  return r;
}

int (*const fold_table[1])(int, int) = {fold_steps};

int fold_a(int a) { return fold_steps(a, 4); }
int fold_b(int a) { return fold_steps(a, 0) + fold_steps(a, 7); }

static void dump(const char *tag, struct pool *p)
{
  printf("%s:", tag);
  for (uintptr_t w = 0; w < (p->pages + 31) / 32; w++)
    printf(" %08lx", (unsigned long)p->map.bits.masks[w]);
  printf(" used=%lu hint=%lu\n", (unsigned long)p->used, (unsigned long)p->hint);
}

int main(void)
{
  static uintptr_t words[4];
  struct pool p = {.pages = 100, .map = {.bits = {100, words}}, .used = 0, .hint = 0};

  claim_range(&p, 0, 0);    /* empty range: no change */
  dump("empty", &p);
  claim_range(&p, 0, 5);    /* same word, from bit 0 */
  dump("c0_5", &p);
  claim_range(&p, 7, 9);    /* same word, inner */
  dump("c7_9", &p);
  claim_range(&p, 30, 32);  /* ends exactly at a word boundary */
  dump("c30_32", &p);
  claim_range(&p, 40, 100); /* across words, ends in the last word */
  dump("c40_100", &p);
  claim_range(&p, 31, 31);  /* empty range inside a word */
  dump("c31_31", &p);

  printf("used 0..100=%lu 0..32=%lu 5..7=%lu 8..41=%lu\n", (unsigned long)used_between(&p, 0, 100),
         (unsigned long)used_between(&p, 0, 32), (unsigned long)used_between(&p, 5, 7),
         (unsigned long)used_between(&p, 8, 41));
  printf("first_set 5..100=%ld 9..30=%ld 99..100=%ld 50..50=%ld\n", first_set(&p, 5, 100), first_set(&p, 9, 30),
         first_set(&p, 99, 100), first_set(&p, 50, 50));
  printf("first_clear 0..100=%ld 5..7=%ld 40..100=%ld 3..5=%ld\n", first_clear(&p, 0, 100), first_clear(&p, 5, 7),
         first_clear(&p, 40, 100), first_clear(&p, 3, 5));
  printf("find_free n=1:%lu n=2:%lu n=3:%lu n=20:%lu n=21:%lu\n", (unsigned long)find_free(&p, 0, 1),
         (unsigned long)find_free(&p, 0, 2), (unsigned long)find_free(&p, 0, 3),
         (unsigned long)find_free(&p, 0, 20), (unsigned long)find_free(&p, 0, 21));

  release_range(&p, 64, 96); /* exactly one whole word */
  dump("r64_96", &p);
  release_range(&p, 3, 8);   /* clears a mix of set and clear bits */
  dump("r3_8", &p);
  release_range(&p, 33, 66); /* across a word boundary from bit 1 */
  dump("r33_66", &p);
  release_range(&p, 10, 10); /* empty */
  dump("r10_10", &p);
  claim_range(&p, 3, 66);    /* hint = 3: the claim moves it */
  dump("c3_66", &p);
  release_range(&p, 0, 100);
  dump("r0_100", &p);
  printf("find_free after clear n=100:%lu n=101:%lu\n", (unsigned long)find_free(&p, 0, 100),
         (unsigned long)find_free(&p, 0, 101));

  printf("walk %u %u %u %u\n", walk_a(1), walk_a(27), walk_b(0), walk_b(9));
  scan_twice(0x50);
  scan_twice(0);
  struct opt_usize cs = counted_scan(0x80, 8);
  printf("scans %u %d %lu\n", scans, cs.is_null, (unsigned long)cs.payload);
  printf("fold %d %d %d %d\n", fold_a(5), fold_b(-3), fold_table[0](5, 4), fold_table[0](-3, 7));
  return 0;
}
