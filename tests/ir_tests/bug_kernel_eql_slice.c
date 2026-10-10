/*
 *  TCC IR - Kernel slice normalization and eql regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Keep the generated slice normalization, size ladder and address-taken
 * copies together: simplified equality loops do not expose these misses. */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define zig_nonstring
#define zig_noinline __attribute__((noinline))
struct arr_4_u8_166 { uint8_t array[4]; };
struct arr_4_usize_24415 { uintptr_t array[4]; };
struct slice_u8_49 { uint8_t const *ptr; uintptr_t len; };
static uint32_t zig_u32_bitCast_u32(uintptr_t v, uint32_t bits) { (void)bits; return (uint32_t)v; }
static uint32_t zig_u32_intCast_u8(uint8_t v) { return (uint32_t)v; }
struct arr_0_u8_13895 { uint8_t array[0]; };
static struct slice_u8_49 mem_sliceAsBytes__anon_43293__16596(struct slice_u8_49 const a0) {
 /* mem.sliceAsBytes__anon_43293 */
 struct slice_u8_49 const *t1;
 uintptr_t const *t2;
 uintptr_t t3;
 uint32_t t4;
 uint8_t const *t7 zig_nonstring;
 uint8_t const *t8 zig_nonstring;
 uint8_t const *const *t9;
 struct slice_u8_49 t10;
 struct slice_u8_49 t0;
 bool t5;
 bool t6;
 t0 = a0;
 t1 = (struct slice_u8_49 const *)&t0;
 /* 9:9 */
 /* 9:14 */
 t2 = &t1->len;
 t3 = (*t2);
 t4 = zig_u32_bitCast_u32(t3, UINT8_C(32));
 t5 = t4 == UINT32_C(0);
 if (t5) {
  /* 9:45 */
  /* inline:meta.sentinel */
  /* 2:13 */
  /* 5:21 */
  /* 6:54 */
  /* inline:lang.Type.Pointer.sentinel */
  /* 2:13 */
  /* 2:86 */
  goto zig_block_3;

zig_block_3:;
  /* 6:34 */
  goto zig_block_2;

zig_block_2:;
  t6 = true;
  goto zig_block_1;
 }
 t6 = false;
 goto zig_block_1;

zig_block_1:;
 if (t6) {
  /* 9:62 */
  return (struct slice_u8_49){((uint8_t const *)((struct arr_0_u8_13895 const *)(uintptr_t)0xaaaaaaaaul)),(uintptr_t)0ul};
 }
 goto zig_block_0;

zig_block_0:;
 /* 13:29 */
 t7 = a0.ptr;
 t8 = t7;
 t9 = (uint8_t const *const *)&t8;
 /* 13:56 */
 t2 = &t1->len;
 t3 = (*t2);
 /* 13:61 */
 t3 = t3 * (uintptr_t)1ul;
 /* 13:45 */
 t7 = (*t9);
 t7 = (uint8_t const *)(((uintptr_t)t7) + ((uintptr_t)0ul*sizeof(uint8_t)));
 t3 = t3 - (uintptr_t)0ul;
 t10.ptr = t7;
 t10.len = t3;
 /* 13:5 */
 return t10;
}
static bool mem_eqlBytes__1647(struct slice_u8_49 const a0, struct slice_u8_49 const a1) {
 /* mem.eqlBytes */
 struct slice_u8_49 const *t1;
 struct slice_u8_49 const *t3;
 uintptr_t const *t4;
 uintptr_t t5;
 uintptr_t t6;
 uintptr_t t21;
 uintptr_t t22;
 uintptr_t t39;
 uintptr_t t20;
 uintptr_t t27;
 uintptr_t t31;
 uintptr_t t34;
 uintptr_t t35;
 uintptr_t t37;
 uintptr_t t40;
 uintptr_t t42;
 uint32_t t7;
 uint32_t t8;
 uint32_t t26;
 uint32_t t29;
 uint32_t t30;
 uint32_t t33;
 uint32_t t19;
 uint32_t t28;
 uint32_t t32;
 uint32_t t36;
 uint32_t t38;
 uint32_t t41;
 uint32_t t43;
 uint8_t const *const *t11;
 uint8_t const *t12;
 uint8_t const *t13;
 struct slice_u8_49 t14;
 struct slice_u8_49 t0;
 struct slice_u8_49 t2;
 uint8_t const *t15;
 struct arr_4_usize_24415 t23;
 struct arr_4_u8_166 const *t24;
 bool t9;
 bool t10;
 uint8_t t16;
 uint8_t t17;
 uint8_t t18;
 struct arr_4_u8_166 t25;
 t0 = a0;
 t1 = (struct slice_u8_49 const *)&t0;
 t2 = a1;
 t3 = (struct slice_u8_49 const *)&t2;
 /* 4:9 */
 /* 4:10 */
 t4 = &t1->len;
 t5 = (*t4);
 /* 4:19 */
 t4 = &t3->len;
 t6 = (*t4);
 t7 = zig_u32_bitCast_u32(t5, UINT8_C(32));
 t8 = zig_u32_bitCast_u32(t6, UINT8_C(32));
 t9 = t7 != t8;
 if (t9) {
  /* 4:25 */
  return false;
 }
 goto zig_block_0;

zig_block_0:;
 /* 5:9 */
 /* 5:10 */
 t4 = &t1->len;
 t6 = (*t4);
 t8 = zig_u32_bitCast_u32(t6, UINT8_C(32));
 t9 = t8 == UINT32_C(0);
 if (t9) {
  t10 = true;
  goto zig_block_2;
 }
 /* 5:24 */
 t11 = &t1->ptr;
 t12 = (*t11);
 /* 5:33 */
 t11 = &t3->ptr;
 t13 = (*t11);
 t9 = t12 == t13;
 t10 = t9;
 goto zig_block_2;

zig_block_2:;
 if (t10) {
  /* 5:39 */
  return true;
 }
 goto zig_block_1;

zig_block_1:;
 /* 7:9 */
 /* 7:10 */
 t4 = &t1->len;
 t6 = (*t4);
 t8 = zig_u32_bitCast_u32(t6, UINT8_C(32));
 t10 = t8 <= UINT32_C(16);
 if (t10) {
  /* 8:13 */
  /* 8:14 */
  t4 = &t1->len;
  t6 = (*t4);
  t8 = zig_u32_bitCast_u32(t6, UINT8_C(32));
  t10 = t8 < UINT32_C(4);
  if (t10) {
   /* 9:25 */
   t14 = (*t1);
   t15 = &t14.ptr[(uintptr_t)0ul];
   t16 = (*t15);
   /* 9:32 */
   t14 = (*t3);
   t15 = &t14.ptr[(uintptr_t)0ul];
   t17 = (*t15);
   t17 = t16 ^ t17;
   /* 9:43 */
   t4 = &t1->len;
   t6 = (*t4);
   /* 9:48 */
   t6 = t6 - (uintptr_t)1ul;
   /* 9:41 */
   t14 = (*t1);
   t15 = &t14.ptr[t6];
   t16 = (*t15);
   /* 9:58 */
   t4 = &t1->len;
   t6 = (*t4);
   /* 9:63 */
   t6 = t6 - (uintptr_t)1ul;
   /* 9:56 */
   t14 = (*t3);
   t15 = &t14.ptr[t6];
   t18 = (*t15);
   t18 = t16 ^ t18;
   t18 = t17 | t18;
   /* 9:75 */
   t4 = &t1->len;
   t6 = (*t4);
   /* 9:80 */
   t6 = t6 / (uintptr_t)2ul;
   /* 9:73 */
   t14 = (*t1);
   t15 = &t14.ptr[t6];
   t17 = (*t15);
   /* 9:90 */
   t4 = &t1->len;
   t6 = (*t4);
   /* 9:95 */
   t6 = t6 / (uintptr_t)2ul;
   /* 9:88 */
   t14 = (*t3);
   t15 = &t14.ptr[t6];
   t16 = (*t15);
   t16 = t17 ^ t16;
   t16 = t18 | t16;
   /* dbg_var_val:x */
   /* 10:13 */
   t10 = t16 == UINT8_C(0);
   /* 10:13 */
   return t10;
  }
  goto zig_block_4;

zig_block_4:;
  /* 12:9 */
  t19 = UINT32_C(0);
  /* dbg_var_ptr:x */
  t20 = (uintptr_t)0ul;
  /* 13:28 */
  t4 = &t1->len;
  t6 = (*t4);
  /* 13:33 */
  t6 = t6 - (uintptr_t)4ul;
  /* 13:40 */
  t4 = &t1->len;
  t5 = (*t4);
  /* 13:45 */
  t5 = t5 / (uintptr_t)8ul;
  /* 13:50 */
  t5 = t5 * (uintptr_t)4ul;
  /* 13:56 */
  t4 = &t1->len;
  t21 = (*t4);
  /* 13:61 */
  t21 = t21 - (uintptr_t)4ul;
  /* 13:70 */
  t4 = &t1->len;
  t22 = (*t4);
  /* 13:75 */
  t22 = t22 / (uintptr_t)8ul;
  /* 13:80 */
  t22 = t22 * (uintptr_t)4ul;
  /* 13:65 */
  t22 = t21 - t22;
  t23.array[0] = (uintptr_t)0ul;
  t23.array[1] = t6;
  t23.array[2] = t5;
  t23.array[3] = t22;
  zig_loop_149:
  t22 = t20;
  t8 = zig_u32_bitCast_u32(t22, UINT8_C(32));
  t10 = t8 < UINT32_C(4);
  if (t10) {
   t5 = t23.array[t22];
   /* dbg_var_val:n */
   /* 14:13 */
   t8 = t19;
   /* 14:42 */
   t14 = (*t1);
   t13 = t14.ptr;
   t13 = (uint8_t const *)(((uintptr_t)t13) + (t5*sizeof(uint8_t)));
   t24 = (struct arr_4_u8_166 const *)t13;
   t25 = (*t24);
   /* 14:27 */
   t27 = (uintptr_t)3ul;
   t28 = UINT32_C(0);
   zig_loop_353:
   t6 = t27;
   t18 = t25.array[t6];
   t29 = zig_u32_intCast_u8(t18);
   t30 = t28;
   t30 = t30 << UINT8_C(8);
   t29 = t30 | t29;
   t10 = t6 == (uintptr_t)0ul;
   if (t10) {
    t26 = t29;
    goto zig_block_8;
   }
   t28 = t29;
   t6 = t6 - (uintptr_t)1ul;
   t27 = t6;
   goto zig_loop_353;

zig_block_8:;
   t7 = t26;
   goto zig_block_7;

zig_block_7:;
   /* 14:79 */
   t14 = (*t3);
   t13 = t14.ptr;
   t13 = (uint8_t const *)(((uintptr_t)t13) + (t5*sizeof(uint8_t)));
   t24 = (struct arr_4_u8_166 const *)t13;
   t25 = (*t24);
   /* 14:64 */
   t31 = (uintptr_t)3ul;
   t32 = UINT32_C(0);
   zig_loop_373:
   t5 = t31;
   t18 = t25.array[t5];
   t26 = zig_u32_intCast_u8(t18);
   t33 = t32;
   t33 = t33 << UINT8_C(8);
   t26 = t33 | t26;
   t10 = t5 == (uintptr_t)0ul;
   if (t10) {
    t29 = t26;
    goto zig_block_10;
   }
   t32 = t26;
   t5 = t5 - (uintptr_t)1ul;
   t31 = t5;
   goto zig_loop_373;

zig_block_10:;
   t30 = t29;
   goto zig_block_9;

zig_block_9:;
   t30 = t7 ^ t30;
   t30 = t8 | t30;
   t19 = t30;
   /* 15:9 */
   (void)0;
   goto zig_block_6;
  }
  goto zig_block_5;

zig_block_6:;
  t22 = t22 + (uintptr_t)1ul;
  t20 = t22;
  goto zig_loop_149;

zig_block_5:;
  /* 16:9 */
  t33 = t19;
  t10 = t33 == UINT32_C(0);
  /* 16:9 */
  return t10;
 }
 goto zig_block_3;

zig_block_3:;
 /* 21:22 */
 /* 38:17 */
 /* dbg_var_val:s */
 /* 40:13 */
 /* 47:5 */
 (void)0;
 goto zig_block_11;

zig_block_11:;
 /* dbg_var_val:s */
 /* 40:13 */
 /* 47:5 */
 (void)0;
 goto zig_block_12;

zig_block_12:;
 /* dbg_var_val:s */
 /* 40:13 */
 /* 47:5 */
 (void)0;
 goto zig_block_13;

zig_block_13:;
 /* dbg_var_val:s */
 /* 40:13 */
 /* 47:5 */
 (void)0;
 goto zig_block_14;

zig_block_14:;
 /* dbg_var_val:s */
 /* 40:13 */
 /* 47:5 */
 (void)0;
 goto zig_block_15;

zig_block_15:;
 t34 = (uintptr_t)0ul;
 /* 49:15 */
 t4 = &t1->len;
 t22 = (*t4);
 /* 49:20 */
 t22 = t22 - (uintptr_t)1ul;
 /* 49:25 */
 t22 = t22 / (uintptr_t)4ul;
 zig_loop_250:
 t21 = t34;
 t33 = zig_u32_bitCast_u32(t21, UINT8_C(32));
 t30 = zig_u32_bitCast_u32(t22, UINT8_C(32));
 t10 = t33 < t30;
 if (t10) {
  /* dbg_var_val:i */
  /* 50:50 */
  t6 = t21 * (uintptr_t)4ul;
  /* 50:65 */
  t14 = (*t1);
  t13 = t14.ptr;
  t13 = (uint8_t const *)(((uintptr_t)t13) + (t6*sizeof(uint8_t)));
  t24 = (struct arr_4_u8_166 const *)t13;
  t25 = (*t24);
  /* 50:37 */
  t35 = (uintptr_t)3ul;
  t36 = UINT32_C(0);
  zig_loop_393:
  t5 = t35;
  t18 = t25.array[t5];
  t33 = zig_u32_intCast_u8(t18);
  t29 = t36;
  t29 = t29 << UINT8_C(8);
  t33 = t29 | t33;
  t10 = t5 == (uintptr_t)0ul;
  if (t10) {
   t30 = t33;
   goto zig_block_19;
  }
  t36 = t33;
  t5 = t5 - (uintptr_t)1ul;
  t35 = t5;
  goto zig_loop_393;

zig_block_19:;
  t5 = zig_u32_bitCast_u32(t30, UINT8_C(32));
  t6 = t5;
  goto zig_block_18;

zig_block_18:;
  /* dbg_var_val:a_chunk */
  /* 51:50 */
  t5 = t21 * (uintptr_t)4ul;
  /* 51:65 */
  t14 = (*t3);
  t13 = t14.ptr;
  t13 = (uint8_t const *)(((uintptr_t)t13) + (t5*sizeof(uint8_t)));
  t24 = (struct arr_4_u8_166 const *)t13;
  t25 = (*t24);
  /* 51:37 */
  t37 = (uintptr_t)3ul;
  t38 = UINT32_C(0);
  zig_loop_414:
  t39 = t37;
  t18 = t25.array[t39];
  t33 = zig_u32_intCast_u8(t18);
  t29 = t38;
  t29 = t29 << UINT8_C(8);
  t33 = t29 | t33;
  t10 = t39 == (uintptr_t)0ul;
  if (t10) {
   t30 = t33;
   goto zig_block_21;
  }
  t38 = t33;
  t39 = t39 - (uintptr_t)1ul;
  t37 = t39;
  goto zig_loop_414;

zig_block_21:;
  t39 = zig_u32_bitCast_u32(t30, UINT8_C(32));
  t5 = t39;
  goto zig_block_20;

zig_block_20:;
  /* dbg_var_val:b_chunk */
  /* 52:13 */
  /* 52:28 */
  /* inline:mem.eqlBytes__struct_43189.isNotEqual */
  /* dbg_arg_inline:chunk_a */
  /* dbg_arg_inline:chunk_b */
  /* 2:17 */
  t30 = zig_u32_bitCast_u32(t6, UINT8_C(32));
  t33 = zig_u32_bitCast_u32(t5, UINT8_C(32));
  t9 = t30 != t33;
  /* 2:17 */
  t10 = t9;
  goto zig_block_23;

zig_block_23:;
  if (t10) {
   /* 52:48 */
   return false;
  }
  goto zig_block_22;

zig_block_22:;
  /* 53:5 */
  (void)0;
  goto zig_block_17;
 }
 goto zig_block_16;

zig_block_17:;
 t21 = t21 + (uintptr_t)1ul;
 t34 = t21;
 goto zig_loop_250;

zig_block_16:;
 /* 56:50 */
 t4 = &t1->len;
 t22 = (*t4);
 /* 56:55 */
 t22 = t22 - (uintptr_t)4ul;
 /* 56:70 */
 t14 = (*t1);
 t13 = t14.ptr;
 t13 = (uint8_t const *)(((uintptr_t)t13) + (t22*sizeof(uint8_t)));
 t24 = (struct arr_4_u8_166 const *)t13;
 t25 = (*t24);
 /* 56:38 */
 t40 = (uintptr_t)3ul;
 t41 = UINT32_C(0);
 zig_loop_435:
 t39 = t40;
 t18 = t25.array[t39];
 t30 = zig_u32_intCast_u8(t18);
 t29 = t41;
 t29 = t29 << UINT8_C(8);
 t30 = t29 | t30;
 t10 = t39 == (uintptr_t)0ul;
 if (t10) {
  t33 = t30;
  goto zig_block_25;
 }
 t41 = t30;
 t39 = t39 - (uintptr_t)1ul;
 t40 = t39;
 goto zig_loop_435;

zig_block_25:;
 t39 = zig_u32_bitCast_u32(t33, UINT8_C(32));
 t22 = t39;
 goto zig_block_24;

zig_block_24:;
 /* dbg_var_val:last_a_chunk */
 /* 57:50 */
 t4 = &t1->len;
 t39 = (*t4);
 /* 57:55 */
 t39 = t39 - (uintptr_t)4ul;
 /* 57:70 */
 t14 = (*t3);
 t13 = t14.ptr;
 t13 = (uint8_t const *)(((uintptr_t)t13) + (t39*sizeof(uint8_t)));
 t24 = (struct arr_4_u8_166 const *)t13;
 t25 = (*t24);
 /* 57:38 */
 t42 = (uintptr_t)3ul;
 t43 = UINT32_C(0);
 zig_loop_456:
 t21 = t42;
 t18 = t25.array[t21];
 t30 = zig_u32_intCast_u8(t18);
 t29 = t43;
 t29 = t29 << UINT8_C(8);
 t30 = t29 | t30;
 t10 = t21 == (uintptr_t)0ul;
 if (t10) {
  t33 = t30;
  goto zig_block_27;
 }
 t43 = t30;
 t21 = t21 - (uintptr_t)1ul;
 t42 = t21;
 goto zig_loop_456;

zig_block_27:;
 t21 = zig_u32_bitCast_u32(t33, UINT8_C(32));
 t39 = t21;
 goto zig_block_26;

zig_block_26:;
 /* dbg_var_val:last_b_chunk */
 /* 58:28 */
 /* inline:mem.eqlBytes__struct_43189.isNotEqual */
 /* dbg_arg_inline:chunk_a */
 /* dbg_arg_inline:chunk_b */
 /* 2:17 */
 t33 = zig_u32_bitCast_u32(t22, UINT8_C(32));
 t30 = zig_u32_bitCast_u32(t39, UINT8_C(32));
 t9 = t33 != t30;
 /* 2:17 */
 t10 = t9;
 goto zig_block_28;

zig_block_28:;
 t10 = !t10;
 /* 58:5 */
 return t10;
}
__attribute__((noinline)) bool kernel_eql(struct slice_u8_49 const a0, struct slice_u8_49 const a1) {
 /* mem.eql__anon_10757 */
 struct slice_u8_49 t2;
 struct slice_u8_49 t3;
 struct slice_u8_49 t0;
 struct slice_u8_49 t1;
 bool t4;
 t0 = a0;
 t1 = a1;
 /* 2:9 */
 /* 2:80 */
 /* inline:meta.hasUniqueRepresentation */
 /* 2:20 */
 /* dbg_var_val:info */
 /* 13:46 */
 goto zig_block_1;

zig_block_1:;
 /* 2:5 */
 goto zig_block_0;

zig_block_0:;
 /* 5:37 */
 t2 = mem_sliceAsBytes__anon_43293__16596(a0);
 /* 5:54 */
 t3 = mem_sliceAsBytes__anon_43293__16596(a1);
 /* 5:24 */
 t4 = mem_eqlBytes__1647(t2, t3);
 /* 5:9 */
 return t4;
}

#include <stdio.h>
int main(void)
{
    uint8_t a[68], b[68];
    for (uintptr_t ao = 0; ao < 4; ++ao) {
        for (uintptr_t bo = 0; bo < 4; ++bo) {
            for (uintptr_t n = 0; n <= 64; ++n) {
                for (uintptr_t i = 0; i < n; ++i)
                    a[ao + i] = b[bo + i] = (uint8_t)(i * 73 + n * 19);
                struct slice_u8_49 x = {a + ao, n}, y = {b + bo, n};
                if (!kernel_eql(x, y) || !kernel_eql(x, x))
                    return 1;
                struct slice_u8_49 shorter = {b + bo, n ? n - 1 : 1};
                if (kernel_eql(x, shorter))
                    return 2;
                for (uintptr_t i = 0; i < n; ++i) {
                    b[bo + i] ^= 0x80;
                    if (kernel_eql(x, y))
                        return 3;
                    b[bo + i] ^= 0x80;
                }
            }
        }
    }
    puts("kernel slice eql: ok");
    return 0;
}
