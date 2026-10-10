/*
 *  TCC Regression - Automatic inlining across shared return tails
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint8_t *ptr; unsigned len; } Range;
typedef struct { Range data, got, thunks; } Layout;
typedef struct { Range f0, f1; } Tuple;

static inline __attribute__((always_inline)) unsigned zig_u32_bitCast_u32(unsigned value, unsigned bits)
{
  (void)bits;
  return value;
}

/* Keep Zig's temporary aggregates so late return-tail sharing is exercised. */
static unsigned *word_at(Layout const a0, uintptr_t const a1) {
 Layout const *t1;
 Range const *t3;
 Range t4;
 Range t5;
 Range t7;
 Range t17;
 Tuple t6;
 uintptr_t t8;
 uintptr_t t10;
 uintptr_t t2;
 uintptr_t const *t9;
 uint32_t t11;
 uint32_t t12;
 uint8_t *const *t14;
 uint8_t *t15;
 unsigned *t16;
 Layout t0;
 bool t13;
 t0 = a0;
 t1 = (Layout const *)&t0;
 t2 = a1;
 t3 = (Range const *)&t1->data;
 t4 = (*t3);
 t3 = (Range const *)&t1->got;
 t5 = (*t3);
 t6.f0 = t4;
 t6.f1 = t5;
 t5 = t6.f0;
 t7 = t5;
 t3 = (Range const *)&t7;
 t8 = t2;
 t9 = &t3->len;
 t10 = (*t9);
 t10 = t10 / (uintptr_t)4ul;
 t11 = zig_u32_bitCast_u32(t8, UINT8_C(32));
 t12 = zig_u32_bitCast_u32(t10, UINT8_C(32));
 t13 = t11 < t12;
 if (t13) {
  t14 = &t3->ptr;
  t15 = (*t14);
  t10 = t2;
  t10 = t10 * (uintptr_t)4ul;
  t15 = (uint8_t *)(((uintptr_t)t15) + (t10*sizeof(uint8_t)));
  t16 = (unsigned *)t15;
  return t16;
 }
 goto zig_block_1;
zig_block_1:;
 t10 = t2;
 t9 = &t3->len;
 t8 = (*t9);
 t8 = t8 / (uintptr_t)4ul;
 t8 = t10 - t8;
 t2 = t8;
 (void)0;
 goto zig_block_0;
zig_block_0:;
 t5 = t6.f1;
 t17 = t5;
 t3 = (Range const *)&t17;
 t10 = t2;
 t9 = &t3->len;
 t8 = (*t9);
 t8 = t8 / (uintptr_t)4ul;
 t12 = zig_u32_bitCast_u32(t10, UINT8_C(32));
 t11 = zig_u32_bitCast_u32(t8, UINT8_C(32));
 t13 = t12 < t11;
 if (t13) {
  t14 = &t3->ptr;
  t15 = (*t14);
  t8 = t2;
  t8 = t8 * (uintptr_t)4ul;
  t15 = (uint8_t *)(((uintptr_t)t15) + (t8*sizeof(uint8_t)));
  t16 = (unsigned *)t15;
  return t16;
 }
 goto zig_block_3;
zig_block_3:;
 t10 = t2;
 t9 = &t3->len;
 t8 = (*t9);
 t8 = t8 / (uintptr_t)4ul;
 t8 = t10 - t8;
 t2 = t8;
 (void)0;
 goto zig_block_2;
zig_block_2:;
 t3 = (Range const *)&t1->thunks;
 t14 = &t3->ptr;
 t15 = (*t14);
 t10 = t2;
 t10 = t10 * (uintptr_t)4ul;
 t15 = (uint8_t *)(((uintptr_t)t15) + (t10*sizeof(uint8_t)));
 t16 = (unsigned *)t15;
 return t16;
}

unsigned *select_word(Layout layout, unsigned index)
{
  return word_at(layout, index);
}

unsigned *select_word_again(Layout layout, unsigned index)
{
  return word_at(layout, index ^ 1);
}

int main(void)
{
  unsigned data[] = {11, 22, 33};
  unsigned got[] = {44, 55};
  unsigned thunks[] = {66, 77, 88, 99};
  Layout layout = {{(uint8_t *)data, sizeof(data)}, {(uint8_t *)got, sizeof(got)}, {(uint8_t *)thunks, sizeof(thunks)}};
  for (unsigned i = 0; i < 9; i++)
    printf("%u%c", *(i & 1 ? select_word(layout, i) : select_word_again(layout, i ^ 1)), i == 8 ? '\n' : ' ');
  return 0;
}
