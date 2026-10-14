/*
 *  TCC IR - a branch threaded past a jump block leaves that jump orphaned
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* ProcessMemoryPool.reserve_pages as Zig's C backend emits it, find_free_run
 * inlined: the "found" test branches to a block of coalesced phi copies and a
 * JUMP that the real pass threads past, so nothing reaches that JUMP.  It was
 * still emitted between the window loop's `bhi <exit>; b <head>` pair and kept
 * the pair from inverting (codegen: an orphaned JUMP is dropped and the hop
 * skips it).  main() pins the paths through the loop. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct AllocVTable;
struct Allocator { void *ptr; struct AllocVTable const *vtable; };
struct BitSet { uintptr_t bit_length; uintptr_t *masks; };
struct BitSetManaged { struct Allocator allocator; struct BitSet unmanaged; };
struct Region {
 uintptr_t start_address; uintptr_t page_count; uintptr_t page_size;
 struct BitSetManaged page_bitmap;
 uintptr_t peak_used; uintptr_t watermark_used; uintptr_t used_pages; uintptr_t first_free_hint;
};
struct OptUsize { uintptr_t payload; bool is_null; };
static void debug_assert(bool a0);
static uintptr_t mask_bit(uintptr_t a0);
static uintptr_t mask_index(uintptr_t a0);
static bool bitset_is_set(struct BitSet a0, uintptr_t a1);
static bool managed_is_set(struct BitSetManaged a0, uintptr_t a1);
static void debug_assert(bool const a0) {
 bool t0;
 t0 = !a0;
 if (t0) {
  __builtin_unreachable();
 }
 goto zig_block_0;

zig_block_0:;
 return;
}
static struct OptUsize find_free_run(struct Region const *const a0, uintptr_t const a1, uintptr_t const a2, uintptr_t const a3) {
 struct Region const *const *t1;
 uint32_t t2;
 uint32_t t6;
 uintptr_t t5;
 uintptr_t t12;
 uintptr_t t4;
 uintptr_t t7;
 struct Region const *t9;
 struct Region const *t0;
 struct BitSetManaged const *t10;
 struct BitSetManaged t11;
 struct OptUsize t13;
 struct OptUsize t8;
 bool t3;
 t0 = a0;
 t1 = (struct Region const *const *)&t0;
 t2 = (uint32_t)(a1);
 t3 = t2 == UINT32_C(0);
 if (t3) {
  return (struct OptUsize){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
 }
 goto zig_block_0;

zig_block_0:;
 t4 = a2;
 zig_loop_21:
 t5 = t4;
 t5 = t5 + a1;
 t2 = (uint32_t)(t5);
 t6 = (uint32_t)(a3);
 t3 = t2 <= t6;
 if (t3) {
  t7 = a1;
  t8 = (struct OptUsize){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
  zig_loop_41:
  t5 = t7;
  t6 = (uint32_t)(t5);
  t3 = t6 > UINT32_C(0);
  if (t3) {
   t5 = t7;
   t5 = t5 - (uintptr_t)1ul;
   t7 = t5;
   t9 = (*t1);
   t10 = (struct BitSetManaged const *)&t9->page_bitmap;
   t11 = (*t10);
   t5 = t4;
   t12 = t7;
   t12 = t5 + t12;
   t3 = managed_is_set(t11, t12);
   if (t3) {
    t12 = t4;
    t5 = t7;
    t5 = t12 + t5;
    t13.is_null = false;
    t13.payload = t5;
    t8 = t13;
    goto zig_block_4;
   }
   goto zig_block_6;

zig_block_6:;
   (void)0;
   goto zig_block_5;
  }
  goto zig_block_4;

zig_block_5:;
  goto zig_loop_41;

zig_block_4:;
  t13 = t8;
  t3 = !t13.is_null;
  if (t3) {
   t12 = t13.payload;
   t12 = t12 + (uintptr_t)1ul;
   t4 = t12;
   goto zig_block_7;
  }
  t12 = t4;
  t13.is_null = false;
  t13.payload = t12;
  return t13;

zig_block_7:;
  (void)0;
  goto zig_block_3;

zig_block_3:;
  goto zig_block_2;
 }
 goto zig_block_1;

zig_block_2:;
 goto zig_loop_21;

zig_block_1:;
 return (struct OptUsize){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
}
static bool managed_is_set(struct BitSetManaged const a0, uintptr_t const a1) {
 struct BitSetManaged const *t1;
 struct BitSet const *t2;
 struct BitSet t3;
 struct BitSetManaged t0;
 bool t4;
 t0 = a0;
 t1 = (struct BitSetManaged const *)&t0;
 t2 = (struct BitSet const *)&t1->unmanaged;
 t3 = (*t2);
 t4 = bitset_is_set(t3, a1);
 return t4;
}
static bool bitset_is_set(struct BitSet const a0, uintptr_t const a1) {
 struct BitSet const *t1;
 uintptr_t const *t2;
 uintptr_t t3;
 uintptr_t t10;
 uint32_t t4;
 uint32_t t5;
 uintptr_t *const *t7;
 uintptr_t *t8;
 uintptr_t *t9;
 struct BitSet t0;
 bool t6;
 t0 = a0;
 t1 = (struct BitSet const *)&t0;
 t2 = (uintptr_t const *)&t1->bit_length;
 t3 = (*t2);
 t4 = (uint32_t)(a1);
 t5 = (uint32_t)(t3);
 t6 = t4 < t5;
 debug_assert(t6);
 t7 = (uintptr_t *const *)&t1->masks;
 t3 = mask_index(a1);
 t8 = (*t7);
 t9 = &t8[t3];
 t3 = (*t9);
 t10 = mask_bit(a1);
 t10 = t3 & t10;
 t5 = (uint32_t)(t10);
 t6 = t5 != UINT32_C(0);
 return t6;
}
static uintptr_t mask_bit(uintptr_t const a0) {
 uintptr_t t1;
 uint8_t t0;
 t0 = (uint8_t)(a0 & 31u);
 t1 = (uintptr_t)1ul << t0;
 return t1;
}
static uintptr_t mask_index(uintptr_t const a0) {
 uintptr_t t0;
 t0 = a0 >> 5;
 return t0;
}

static int records;
__attribute__((noinline)) void record(uintptr_t a, uint32_t b) { records += 1 + (int)a + (int)b; }
__attribute__((noinline)) void mark_used(struct Region *r, uintptr_t s, uintptr_t e) { r->used_pages += e - s; }
__attribute__((noinline)) uintptr_t reserve(struct Region *regions, uintptr_t nregions, uintptr_t pages) {
 uintptr_t t9, t33, t20 = pages, t4 = 0, t21, t30, t31, t14;
 bool t32;
 struct OptUsize t36;
 zig_loop_28:
 if (t4 < nregions) {
  struct Region *t16 = &regions[t4];
  t14 = t16->page_count;
  t30 = t16->first_free_hint;
  zig_loop_128:
  if (t30 < t14) {
   t31 = t16->page_bitmap.unmanaged.masks[t30 / 32u];
   t32 = (t31 & ((uintptr_t)1 << (t30 % 32u))) != 0;
   goto zig_block_7;
  }
  t32 = false;
  goto zig_block_7;
 zig_block_7:;
  if (t32) { t30 = t30 + 1; goto zig_loop_128; }
  t16->first_free_hint = t30;
  t21 = t30;
  t36 = find_free_run(t16, t20, t21, t16->page_count);
  t32 = !t36.is_null;
  if (t32) {
   t9 = t36.payload;
   t33 = t9;
   goto zig_block_8;
  }
  goto zig_block_1;
 zig_block_8:;
  record(0, 0);
  t9 = t33 + t20;
  mark_used(t16, t33, t9);
  return t33;
 zig_block_1:;
  t4 = t4 + 1;
  goto zig_loop_28;
 }
 return (uintptr_t)-1;
}

static uintptr_t bits_full[2] = { 0xffffffffu, 0xffffffffu };
static uintptr_t bits_mixed[4] = { 0x0000ff0fu, 0x00000000u, 0x00f00000u, 0xffffffffu };
static uintptr_t bits_tail[1] = { 0x7fffffffu };
static uintptr_t bits_comb[2] = { 0x01010101u, 0x01010101u };

static void init(struct Region *r, uintptr_t pages, uintptr_t *bits, uintptr_t hint)
{
  r->start_address = 0x1000;
  r->page_count = pages;
  r->page_size = 4096;
  r->page_bitmap.allocator.ptr = 0;
  r->page_bitmap.allocator.vtable = 0;
  r->page_bitmap.unmanaged.bit_length = pages;
  r->page_bitmap.unmanaged.masks = bits;
  r->peak_used = r->watermark_used = r->used_pages = 0;
  r->first_free_hint = hint;
}

static void show(const char *what, uintptr_t got, struct Region *regs, int n)
{
  printf("%s %d", what, (int)got);
  for (int i = 0; i < n; i++)
    printf(" [%u %u]", (unsigned)regs[i].first_free_hint, (unsigned)regs[i].used_pages);
  printf(" rec %d\n", records);
}

int main(void)
{
  struct Region regs[3];
  init(&regs[0], 64, bits_full, 0);
  init(&regs[1], 128, bits_mixed, 0);
  init(&regs[2], 32, bits_tail, 0);
  /* region 0 is full; region 1's first 4-run starts at 4 */
  show("four", reserve(regs, 3, 4), regs, 3);
  /* windows blocked until bit 24 of word 0 */
  init(&regs[1], 128, bits_mixed, 0);
  show("eight", reserve(regs, 3, 8), regs, 3);
  /* spans words 1 and 2 up to the 0x00f00000 run */
  init(&regs[1], 128, bits_mixed, 0);
  show("sixty", reserve(regs, 3, 60), regs, 3);
  /* blocked at bit 84, the next window runs past the limit; region 2 is 32 pages */
  init(&regs[1], 128, bits_mixed, 0);
  show("big", reserve(regs, 3, 70), regs, 3);
  /* every eighth page taken: 7-runs fit, an 8-run walks every window out */
  init(&regs[0], 64, bits_comb, 0);
  show("comb7", reserve(regs, 1, 7), regs, 1);
  init(&regs[0], 64, bits_comb, 0);
  show("comb8", reserve(regs, 1, 8), regs, 1);
  init(&regs[0], 64, bits_comb, 40);
  show("comb7h", reserve(regs, 1, 7), regs, 1);
  /* a run ending exactly at the limit (bit 31 of region 2) */
  show("last", reserve(regs + 2, 1, 1), regs + 2, 1);
  /* hint past every set bit, and the hint at the limit */
  init(&regs[2], 32, bits_tail, 31);
  show("hint", reserve(regs + 2, 1, 1), regs + 2, 1);
  init(&regs[2], 32, bits_tail, 32);
  show("end", reserve(regs + 2, 1, 1), regs + 2, 1);
  /* zero pages and no regions */
  show("zero", reserve(regs, 3, 0), regs, 3);
  show("none", reserve(regs, 0, 1), regs, 0);
  return 0;
}
