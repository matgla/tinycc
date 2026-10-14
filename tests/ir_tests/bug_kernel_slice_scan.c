/*
 *  TCC IR - Kernel optional-index scalar scan regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Preserve the generated optional-index return and slice address-taking. */
#include <stdint.h>
#include <stdbool.h>
#define zig_nonstring
struct slice_u8_49 { const uint8_t *ptr; uintptr_t len; };
struct opt_usize_673 { uintptr_t payload; bool is_null; };
static uint32_t zig_u32_bitCast_u32(uintptr_t v, uint32_t bits) { (void)bits; return v; }
static bool math_isPowerOfTwo__anon_2129__2441(void) { return true; }
static struct opt_usize_673 find_pos(struct slice_u8_49 const a0, uintptr_t const a1, uint8_t const a2) {
 /* mem.findScalarPos__anon_1363 */
 struct slice_u8_49 const *t1;
 uintptr_t const *t2;
 uintptr_t t3;
 uintptr_t t11;
 uintptr_t t12;
 uintptr_t t14;
 uintptr_t t7;
 uintptr_t t8;
 uint32_t t4;
 uint32_t t5;
 struct slice_u8_49 t9;
 struct slice_u8_49 t0;
 uint8_t const *t10 zig_nonstring;
 struct opt_usize_673 t15;
 bool t6;
 uint8_t t13;
 t0 = a0;
 t1 = (struct slice_u8_49 const *)&t0;
 /* 2:9 */
 /* 2:29 */
 t2 = &t1->len;
 t3 = (*t2);
 t4 = zig_u32_bitCast_u32(a1, UINT8_C(32));
 t5 = zig_u32_bitCast_u32(t3, UINT8_C(32));
 t6 = t4 >= t5;
 if (t6) {
  /* 2:35 */
  return (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
 }
 goto zig_block_0;

zig_block_0:;
 /* 4:5 */
 t7 = a1;
 /* dbg_var_ptr:i */
 /* 5:9 */
 /* 6:30 */
 /* inline:debug.inValgrind */
 /* 3:9 */
 /* 3:36 */
 goto zig_block_2;

zig_block_2:;
 /* 8:83 */
 t6 = math_isPowerOfTwo__anon_2129__2441();
 if (t6) {
  /* 10:13 */
  /* 10:41 */
  goto zig_block_1;
 }
 goto zig_block_1;

zig_block_1:;
 t8 = (uintptr_t)0ul;
 /* 57:10 */
 t3 = t7;
 /* 57:15 */
 t9 = (*t1);
 t10 = t9.ptr;
 t10 = (uint8_t const *)(((uintptr_t)t10) + (t3*sizeof(uint8_t)));
 t11 = t9.len;
 t3 = t11 - t3;
 t9.ptr = t10;
 t9.len = t3;
 /* 57:22 */
 t3 = t7;
 t11 = t9.len;
 zig_loop_71:
 t12 = t8;
 t5 = zig_u32_bitCast_u32(t12, UINT8_C(32));
 t4 = zig_u32_bitCast_u32(t11, UINT8_C(32));
 t6 = t5 < t4;
 if (t6) {
  t13 = t9.ptr[t12];
  /* dbg_var_val:c */
  t14 = t3 + t12;
  /* dbg_var_val:j */
  /* 58:13 */
  t6 = t13 == a2;
  if (t6) {
   /* 58:25 */
   t15.is_null = false;
   t15.payload = t14;
   return t15;
  }
  goto zig_block_5;

zig_block_5:;
  /* 59:5 */
  (void)0;
  goto zig_block_4;
 }
 goto zig_block_3;

zig_block_4:;
 t12 = t12 + (uintptr_t)1ul;
 t8 = t12;
 goto zig_loop_71;

zig_block_3:;
 /* 60:5 */
 return (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
}
__attribute__((noinline)) uintptr_t scan_payload(const uint8_t *p, uintptr_t n, uintptr_t start, uint8_t needle)
{
  struct opt_usize_673 r = find_pos((struct slice_u8_49){p, n}, start, needle);
  return r.is_null ? n : r.payload;
}
__attribute__((noinline)) bool scan_present(const uint8_t *p, uintptr_t n, uintptr_t start, uint8_t needle)
{
  return !find_pos((struct slice_u8_49){p, n}, start, needle).is_null;
}
__attribute__((noinline)) unsigned scan_three(const uint8_t *p, uintptr_t n)
{
  unsigned a = find_pos((struct slice_u8_49){p, n}, 0, 1).is_null;
  unsigned b = find_pos((struct slice_u8_49){p, n}, 0, 2).is_null;
  unsigned c = find_pos((struct slice_u8_49){p, n}, 0, 3).is_null;
  return a + b + c;
}
#include <stdio.h>
int main(void)
{
  uint8_t data[72];
  for (unsigned i = 0; i < sizeof(data); i++) data[i] = i % 7;
  for (unsigned alignment = 0; alignment < 4; alignment++)
    for (unsigned n = 0; n <= 64; n++)
      for (unsigned start = 0; start <= n + 2; start++)
        for (unsigned needle = 0; needle <= 7; needle++)
        {
          unsigned expected = n;
          for (unsigned i = start; i < n; i++)
            if (data[alignment + i] == needle) { expected = i; break; }
          if (scan_payload(data + alignment, n, start, needle) != expected ||
              scan_present(data + alignment, n, start, needle) != (expected != n))
            return 1;
        }
  if (scan_payload(0, 0, 0, 0) != 0 || scan_present(0, 0, 1, 0)) return 2;
  for (unsigned n = 0; n <= 8; n++)
  {
    unsigned expected = 0;
    for (unsigned needle = 1; needle <= 3; needle++)
    {
      unsigned found = 0;
      for (unsigned i = 0; i < n; i++) found |= data[i] == needle;
      expected += !found;
    }
    if (scan_three(data, n) != expected) return 3;
  }
  puts("slice scan ok");
  return 0;
}
