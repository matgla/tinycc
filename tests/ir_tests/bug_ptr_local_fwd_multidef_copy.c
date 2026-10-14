/*
 *  TCC IR - &local copied through reused (multi-def) pointer locals
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ptr_local_fwd followed `&V` only through single-def pointer copies; the Zig
 * C backend reuses one pointer local per type (`t101 = t100; t74 = t101; *t74`
 * at every field read), so the record took a strd/add/ldrd stack round-trip.
 * Fix: a copy VAR whose defs all copy a VAR resolving to the same V resolves
 * to V too.  Copies naming different locals must keep their own targets. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define zig_noinline __attribute__((noinline))

typedef uint64_t DataRelocation; /* packed struct(u64) { to: u32, from: u30, section: u2 } */
struct slice_DataRelocation { DataRelocation *ptr; uintptr_t len; };
struct Header { uint32_t pad[30]; uint32_t const_rodata_length; uint32_t got_length; };
struct errunion_usize { uintptr_t payload; uint16_t error; };
struct errunion_void { uint16_t error; };

static inline uint64_t zig_shr_u64(uint64_t v, uint8_t s) { return v >> s; }
static inline uint32_t zig_u32_truncate_u64(uint64_t v, uint8_t b) { return (uint32_t)v & (b == 32 ? 0xffffffffu : ((1u << b) - 1)); }
static inline uint8_t zig_u8_truncate_u64(uint64_t v, uint8_t b) { return (uint8_t)v & (uint8_t)((1u << b) - 1); }

#define REREAD() do { t101 = t100; t74 = t101; t75 = (*t74); } while (0) /* CBE spelling */

static struct errunion_usize span_end(uintptr_t const a0, uintptr_t const a1, uintptr_t const a2, uintptr_t const a3)
{
  uintptr_t t5; bool t2, t3, t4; struct errunion_usize t6;
  t2 = a0 > a3;
  if (t2) { t3 = true; goto b1; }
  t2 = a2 != 0;
  if (t2) { t5 = a3 - a0; t5 = t5 / a2; t2 = a1 > t5; t4 = t2; goto b2; }
  t4 = false;
b2: t3 = t4;
b1: if (t3) return (struct errunion_usize){ .error = 67, .payload = 0xaaaaaaaau };
  t5 = a1 * a2; t5 = a0 + t5;
  t6.payload = t5; t6.error = 0;
  return t6;
}

/* The defect site: kernel.c parser_Parser_validate's data_relocations loop. */
zig_noinline struct errunion_void validate_data_relocs(struct Header const *h_in, struct slice_DataRelocation const *relocs, uintptr_t data_bss)
{
  struct Header const *t9 = h_in;
  struct Header const *const *t10 = &t9;
  struct Header const *t8;
  DataRelocation t98, t99;
  DataRelocation const *t100, *t101;
  uint64_t const *t74;
  uint64_t t75;
  uint32_t t20, t21;
  uint8_t t92, t93;
  uintptr_t t31, t33, t94 = 0;
  bool t5, t14;
  struct slice_DataRelocation t97 = *relocs;
  struct errunion_usize t41;
  t31 = t97.len;
loop:
  t33 = t94;
  t14 = t33 < t31;
  if (t14) {
    t98 = t97.ptr[t33];
    t99 = t98;
    t100 = (DataRelocation const *)&t99;
    REREAD();
    t21 = zig_u32_truncate_u64(zig_shr_u64(t75, 0), 32);
    t8 = *t10;
    memcpy(&t20, &t8->const_rodata_length, sizeof(uint32_t));
    t14 = t21 < t20;
    if (t14) { t5 = true; goto b44; }
    REREAD();
    t20 = zig_u32_truncate_u64(zig_shr_u64(t75, 0), 32);
    t20 = t20 % 4u;
    t5 = t20 != 0;
b44:
    if (t5) return (struct errunion_void){ .error = 67 };
    REREAD();
    t21 = zig_u32_truncate_u64(zig_shr_u64(t75, 0), 32);
    t41 = span_end(t21, 1, 4, data_bss);
    if (t41.error) return (struct errunion_void){ .error = t41.error };
    REREAD();
    t92 = zig_u8_truncate_u64(zig_shr_u64(t75, 32), 2);
    t93 = t92;
    t14 = t93 == 3;
    if (t14) {
      REREAD();
      t21 = zig_u32_truncate_u64(zig_shr_u64(t75, 34), 30);
      t8 = *t10;
      memcpy(&t20, &t8->got_length, sizeof(uint32_t));
      t20 = t20 / 8u;
      t5 = t21 >= t20;
    } else
      t5 = false;
    if (t5) return (struct errunion_void){ .error = 68 };
    t33 = t33 + 1;
    t94 = t33;
    goto loop;
  }
  return (struct errunion_void){ .error = 0 };
}

/* Kernel shape: one reused copy local (t74, t80) re-reads records of two
 * different loops, so it names t70 in one loop and t99 in the next and only
 * a per-use (flow-sensitive) resolution applies. */
zig_noinline uint32_t two_tables(DataRelocation const *syms, uintptr_t nsyms,
                                 DataRelocation const *data, uintptr_t ndata)
{
  DataRelocation t69, t70, t98, t99;
  DataRelocation const *t71, *t100, *t101;
  uint64_t const *t72, *t74, *t80;
  uint32_t acc = 0;
  for (uintptr_t i = 0; i < nsyms; i++) {
    t69 = syms[i];
    t70 = t69;
    t71 = &t70;
    t72 = t71; t74 = t72; acc += (uint32_t)*t74;
    t80 = t71; t74 = t80; acc ^= (uint32_t)(*t74 >> 32);
  }
  for (uintptr_t i = 0; i < ndata; i++) {
    t98 = data[i];
    t99 = t98;
    t100 = &t99;
    t101 = t100; t74 = t101; acc = acc * 3 + (uint32_t)*t74;
    t101 = t100; t74 = t101; acc += (uint32_t)(*t74 >> 34);
    if ((uint32_t)(*t74 >> 32) & 3) {
      t80 = t100; t74 = t80; acc ^= (uint32_t)*t74 << 1;
    }
  }
  return acc;
}

/* The local is rewritten between two reads through the reused copy. */
zig_noinline uint64_t reread_after_store(uint64_t const *src, int n)
{
  uint64_t t0, acc = 0;
  uint64_t const *t1, *t2, *t3;
  for (int i = 0; i < n; i++) {
    t0 = src[i];
    t1 = &t0;
    t2 = t1; t3 = t2; acc += *t3;
    t0 = t0 * 3 + 1;
    t2 = t1; t3 = t2; acc ^= *t3;
  }
  return acc;
}

/* Must not be forced: the reused copy names two different locals. */
zig_noinline uint64_t two_targets(uint64_t x, uint64_t y)
{
  uint64_t a = x, b = y;
  uint64_t const *pa = &a, *pb = &b, *c;
  uint64_t r;
  c = pa; r = *c;
  c = pb; r = r * 7 + *c;
  c = pa; r = r * 7 + *c;
  return r;
}

/* Must not be forced: the targets differ per path into the merge. */
zig_noinline uint64_t merge_targets(int sel, uint64_t x, uint64_t y)
{
  uint64_t a = x, b = y;
  uint64_t const *pa = &a, *pb = &b, *c;
  if (sel) c = pa; else c = pb;
  return *c;
}

static uint64_t g_other = 0x1111222233334444ull;
zig_noinline static uint64_t const *other_ptr(void) { return &g_other; }

/* Must not be forced: one def of the reused copy is not a copy of &t0. */
zig_noinline uint64_t non_copy_def(uint64_t x)
{
  uint64_t t0 = x, r;
  uint64_t const *t1 = &t0, *c;
  c = t1; r = *c;
  c = other_ptr(); r += *c;
  c = t1; r += *c;
  return r;
}

/* Volatile source: the copies into the unread `d` read a volatile local and
 * must survive (with the volatile store) while the loop's `q = p` resolves. */
zig_noinline int volatile_copy_src(int n)
{
  int a = 1, b = 2, s = 0;
  int *p, *q, *d;
  int *volatile vp = &b;
  p = &a;
  for (int i = 0; i < n; i++) { q = p; s += *q; a++; }
  d = vp; d = vp; d = vp;
  (void)d;
  return s;
}

int main(void)
{
  static struct Header h;
  static DataRelocation ok[4], bad_align[2], bad_got[2], bad_ro[1];
  struct slice_DataRelocation s;
  h.const_rodata_length = 16;
  h.got_length = 64;
  ok[0] = 16 | ((uint64_t)0 << 32);
  ok[1] = 20 | ((uint64_t)(3 | (5u << 2)) << 32);
  ok[2] = 24 | ((uint64_t)1 << 32);
  ok[3] = 96 | ((uint64_t)(3 | (7u << 2)) << 32);
  bad_align[0] = 16; bad_align[1] = 18;
  bad_got[0] = 16; bad_got[1] = 32 | ((uint64_t)(3 | (8u << 2)) << 32);
  bad_ro[0] = 8;
  s.ptr = ok; s.len = 4;
  printf("ok=%u\n", validate_data_relocs(&h, &s, 100).error);
  printf("span=%u\n", validate_data_relocs(&h, &s, 99).error);
  s.ptr = bad_align; s.len = 2;
  printf("align=%u\n", validate_data_relocs(&h, &s, 100).error);
  s.ptr = bad_got; s.len = 2;
  printf("got=%u\n", validate_data_relocs(&h, &s, 100).error);
  s.ptr = bad_ro; s.len = 1;
  printf("ro=%u\n", validate_data_relocs(&h, &s, 100).error);
  s.len = 0;
  printf("empty=%u\n", validate_data_relocs(&h, &s, 100).error);

  uint64_t src[3] = {5, 0x100000001ull, 77};
  printf("tables=%u %u\n", two_tables(ok, 4, bad_got, 2), two_tables(bad_align, 2, ok, 4));
  printf("reread=%llu\n", (unsigned long long)reread_after_store(src, 3));
  printf("two=%llu\n", (unsigned long long)two_targets(3, 5));
  printf("merge=%llu %llu\n", (unsigned long long)merge_targets(1, 11, 22),
         (unsigned long long)merge_targets(0, 11, 22));
  printf("noncopy=%llu\n", (unsigned long long)non_copy_def(9));
  printf("volsrc=%d\n", volatile_copy_src(4));
  return 0;
}
