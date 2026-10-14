/*
 *  TCC IR - nested-inline budget charged for bounded aggregate-return loops
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The kernel's RomFs.get_file_header expands a called-once walk whose
 * ComponentIterator.init (a pure aggregate-return loop, inline:sret_loop) was
 * charged to the nested-inline budget by its tokens, so peekNext no longer fit
 * inside next.  That class is bounded per caller already; it is no longer
 * charged by tokens as well (call.c inline_body_budget_free). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define zig_u32_bitCast_u32(v, b) ((uint32_t)(v))

struct slice_u8 { uint8_t const *ptr; uintptr_t len; };
struct CompIter { struct slice_u8 path; uintptr_t root_len; uintptr_t root_end_index; uintptr_t start_index; uintptr_t end_index; };
struct Component { struct slice_u8 name; struct slice_u8 path; };
struct opt_Component { struct Component payload; bool is_null; };

static uint8_t nativeToLittle(uint8_t const a0) { return a0; }

static struct opt_Component peekNext(struct CompIter const a0) {
 struct CompIter const *t1; uintptr_t const *t3; uintptr_t t4, t6, t2, t16;
 struct slice_u8 const *t5; uint32_t t7, t8; struct slice_u8 t11; uint8_t const *t12;
 struct Component *t18; struct slice_u8 *t19; uint8_t const *t20;
 struct CompIter t0; struct opt_Component t17; bool t9, t10, t15; uint8_t t13, t14;
 t0 = a0;
 t1 = (struct CompIter const *)&t0;
 t3 = (uintptr_t const *)&t1->end_index;
 t4 = (*t3);
 t2 = t4;
 zig_loop_13:
 t4 = t2;
 t5 = (struct slice_u8 const *)&t1->path;
 t3 = &t5->len;
 t6 = (*t3);
 t7 = zig_u32_bitCast_u32(t4, UINT8_C(32));
 t8 = zig_u32_bitCast_u32(t6, UINT8_C(32));
 t9 = t7 < t8;
 if (t9) {
  t5 = (struct slice_u8 const *)&t1->path;
  t6 = t2;
  t11 = (*t5);
  t12 = &t11.ptr[t6];
  t13 = (*t12);
  t14 = nativeToLittle(UINT8_C(47));
  t15 = t13 == t14;
  t9 = t15;
  goto zig_block_3;
zig_block_3:;
  t10 = t9;
  goto zig_block_2;
 }
 t10 = false;
 goto zig_block_2;
zig_block_2:;
 if (t10) {
  t6 = t2;
  t6 = t6 + (uintptr_t)1ul;
  t2 = t6;
  goto zig_block_1;
 }
 goto zig_block_0;
zig_block_1:;
 goto zig_loop_13;
zig_block_0:;
 t6 = t2;
 t16 = t6;
 zig_loop_69:
 t6 = t16;
 t5 = (struct slice_u8 const *)&t1->path;
 t3 = &t5->len;
 t4 = (*t3);
 t8 = zig_u32_bitCast_u32(t6, UINT8_C(32));
 t7 = zig_u32_bitCast_u32(t4, UINT8_C(32));
 t15 = t8 < t7;
 if (t15) {
  t5 = (struct slice_u8 const *)&t1->path;
  t4 = t16;
  t11 = (*t5);
  t12 = &t11.ptr[t4];
  t14 = (*t12);
  t13 = nativeToLittle(UINT8_C(47));
  t9 = t14 == t13;
  t15 = t9;
  goto zig_block_7;
zig_block_7:;
  t15 = !t15;
  t10 = t15;
  goto zig_block_6;
 }
 t10 = false;
 goto zig_block_6;
zig_block_6:;
 if (t10) {
  t4 = t16;
  t4 = t4 + (uintptr_t)1ul;
  t16 = t4;
  goto zig_block_5;
 }
 goto zig_block_4;
zig_block_5:;
 goto zig_loop_69;
zig_block_4:;
 t6 = t2;
 t4 = t16;
 t8 = zig_u32_bitCast_u32(t6, UINT8_C(32));
 t7 = zig_u32_bitCast_u32(t4, UINT8_C(32));
 t15 = t8 == t7;
 if (t15) {
  return (struct opt_Component){ .is_null = true, .payload = {{((uint8_t const *)(uintptr_t)0xaaaaaaaaul),(uintptr_t)0xaaaaaaaaul},{((uint8_t const *)(uintptr_t)0xaaaaaaaaul),(uintptr_t)0xaaaaaaaaul}} };
 }
 goto zig_block_8;
zig_block_8:;
 t17.is_null = false;
 t18 = &t17.payload;
 t19 = (struct slice_u8 *)&t18->name;
 t5 = (struct slice_u8 const *)&t1->path;
 t6 = t2;
 t4 = t16;
 t11 = (*t5);
 t20 = t11.ptr;
 t20 = (uint8_t const *)(((uintptr_t)t20) + (t6*sizeof(uint8_t)));
 t6 = t4 - t6;
 t11.ptr = t20;
 t11.len = t6;
 (*t19) = t11;
 t19 = (struct slice_u8 *)&t18->path;
 t5 = (struct slice_u8 const *)&t1->path;
 t6 = t16;
 t11 = (*t5);
 t20 = t11.ptr;
 t20 = (uint8_t const *)(((uintptr_t)t20) + ((uintptr_t)0ul*sizeof(uint8_t)));
 t6 = t6 - (uintptr_t)0ul;
 t11.ptr = t20;
 t11.len = t6;
 (*t19) = t11;
 return t17;
}

static struct opt_Component next(struct CompIter *const a0) {
 struct CompIter *const *t1; struct Component t2, t7, t8; struct CompIter *t3, *t0, t4;
 struct opt_Component t5; struct Component const *t9; uintptr_t *t10;
 struct slice_u8 const *t11; uintptr_t const *t12; uintptr_t t13, t14; bool t6;
 t0 = a0;
 t1 = (struct CompIter *const *)&t0;
 t3 = (*t1);
 t4 = (*t3);
 t5 = peekNext(t4);
 t6 = !t5.is_null;
 if (t6) {
  t7 = t5.payload;
  t2 = t7;
  goto zig_block_0;
 }
 return (struct opt_Component){ .is_null = true, .payload = {{((uint8_t const *)(uintptr_t)0xaaaaaaaaul),(uintptr_t)0xaaaaaaaaul},{((uint8_t const *)(uintptr_t)0xaaaaaaaaul),(uintptr_t)0xaaaaaaaaul}} };
zig_block_0:;
 t8 = t2;
 t9 = (struct Component const *)&t8;
 t3 = (*t1);
 t10 = (uintptr_t *)&t3->start_index;
 t11 = (struct slice_u8 const *)&t9->path;
 t12 = &t11->len;
 t13 = (*t12);
 t11 = (struct slice_u8 const *)&t9->name;
 t12 = &t11->len;
 t14 = (*t12);
 t14 = t13 - t14;
 (*t10) = t14;
 t3 = (*t1);
 t10 = (uintptr_t *)&t3->end_index;
 t11 = (struct slice_u8 const *)&t9->path;
 t12 = &t11->len;
 t14 = (*t12);
 (*t10) = t14;
 t5.is_null = false;
 t5.payload = t2;
 return t5;
}

#define PAD4 (void)0; (void)0; (void)0; (void)0;
#define PAD32 PAD4 PAD4 PAD4 PAD4 PAD4 PAD4 PAD4 PAD4
#define PAD160 PAD32 PAD32 PAD32 PAD32 PAD32

/* Zig's ComponentIterator.init, reduced: skip the leading separators.  The
 * padding stands for the C backend's temporaries: tokens, no ops. */
#define DEF_INIT(name, sep)                                                    \
  static struct CompIter name(uint8_t const *p, uintptr_t len)                 \
  {                                                                            \
    struct CompIter r;                                                         \
    uintptr_t i = 0;                                                           \
    PAD160                                                                     \
    while (i < len && p[i] == (sep))                                           \
      i++;                                                                     \
    r.path.ptr = p;                                                            \
    r.path.len = len;                                                          \
    r.root_len = 0;                                                            \
    r.root_end_index = i;                                                      \
    r.start_index = i;                                                         \
    r.end_index = i;                                                           \
    return r;                                                                  \
  }
DEF_INIT(init_a, '/')
DEF_INIT(init_b, '.')
DEF_INIT(init_c, '-')
DEF_INIT(init_d, '_')

/* Called once: its expansions are nested ones, under the caller's budget. */
static uintptr_t walk(uint8_t const *p, uintptr_t len)
{
  struct CompIter a = init_a(p, len);
  struct CompIter b = init_b(p, len);
  struct CompIter c = init_c(p, len);
  struct CompIter d = init_d(p, len);
  uintptr_t n = b.start_index + c.start_index * 10 + d.start_index * 100;
  for (;;)
  {
    struct opt_Component x = next(&a);
    if (x.is_null)
      break;
    n += 1000 * x.payload.name.len;
  }
  return n;
}

uintptr_t lookup(uint8_t const *p, uintptr_t len) { return walk(p, len); }

/* Second sites of the inits: none is called once. */
uintptr_t prefixes(uint8_t const *p, uintptr_t len)
{
  return init_a(p, len).start_index + init_b(p, len).start_index * 10 + init_c(p, len).start_index * 100 +
         init_d(p, len).start_index * 1000;
}

int main(void)
{
  static const char *paths[] = {"/usr/bin/tcc", "//a//bc/", "", "...x", "--y/zz", "__/q"};
  for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
  {
    uint8_t const *p = (uint8_t const *)paths[i];
    uintptr_t len = strlen(paths[i]);
    printf("%u %lu %lu\n", i, (unsigned long)lookup(p, len), (unsigned long)prefixes(p, len));
  }
  return 0;
}
