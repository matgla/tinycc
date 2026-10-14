/*
 *  TCC IR - mem.eql entry: argument copies, unrolled accumulator, frame
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Zig's std.mem.eql(u8) and std.mem.trim as the C backend emits them
 * (kernel.c).  The incoming slices keep the argument registers (ra:co_hint);
 * the late unroll's `T <- #0` index and `0 | x` accumulator fold
 * (const_prop_tmp after sl_forward, identity fold), leaving no frame; each arm
 * returns in r0 instead of through the bool carrier (ra:return_sink); and
 * VRP reads `CMP #0,x` and `x >u 0` facts so trim's zero-trip guard is tested
 * once.  test_codegen_asm.py::test_mem_eql_entry_shape locks the shape. */

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
static bool mem_eql_generic(struct slice_u8_49 const a0, struct slice_u8_49 const a1) {
 struct slice_u8_49 t2;
 struct slice_u8_49 t3;
 bool t4;
 goto zig_block_0;

zig_block_0:;
 t2 = mem_sliceAsBytes__anon_43293__16596(a0);
 t3 = mem_sliceAsBytes__anon_43293__16596(a1);
 t4 = mem_eqlBytes__1647(t2, t3);
 return t4;
}

/* std.mem.trim: the inlined findScalarPos guard `0 >= len` (a CMP #0,x)
 * and the rotated loop's own `0 < len` test must not both survive. */
struct opt_usize_673 { uintptr_t payload; bool is_null; };
static bool math_isPowerOfTwo__anon_2129__2441(void) { return true; }
static struct opt_usize_673 mem_findScalarPos__anon_1363__1906(struct slice_u8_49 const a0, uintptr_t const a1, uint8_t const a2) {
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
 t2 = &t1->len;
 t3 = (*t2);
 t4 = zig_u32_bitCast_u32(a1, UINT8_C(32));
 t5 = zig_u32_bitCast_u32(t3, UINT8_C(32));
 t6 = t4 >= t5;
 if (t6) {
  return (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
 }
 goto zig_block_0;

zig_block_0:;
 t7 = a1;
 goto zig_block_2;

zig_block_2:;
 t6 = math_isPowerOfTwo__anon_2129__2441();
 if (t6) {
  goto zig_block_1;
 }
 goto zig_block_1;

zig_block_1:;
 t8 = (uintptr_t)0ul;
 t3 = t7;
 t9 = (*t1);
 t10 = t9.ptr;
 t10 = (uint8_t const *)(((uintptr_t)t10) + (t3*sizeof(uint8_t)));
 t11 = t9.len;
 t3 = t11 - t3;
 t9.ptr = t10;
 t9.len = t3;
 t3 = t7;
 t11 = t9.len;
 zig_loop_71:
 t12 = t8;
 t5 = zig_u32_bitCast_u32(t12, UINT8_C(32));
 t4 = zig_u32_bitCast_u32(t11, UINT8_C(32));
 t6 = t5 < t4;
 if (t6) {
  t13 = t9.ptr[t12];
  t14 = t3 + t12;
  t6 = t13 == a2;
  if (t6) {
   t15.is_null = false;
   t15.payload = t14;
   return t15;
  }
  goto zig_block_5;

zig_block_5:;
  (void)0;
  goto zig_block_4;
 }
 goto zig_block_3;

zig_block_4:;
 t12 = t12 + (uintptr_t)1ul;
 t8 = t12;
 goto zig_loop_71;

zig_block_3:;
 return (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
}
static struct opt_usize_673 mem_findScalar__anon_2086__2440(struct slice_u8_49 const a0, uint8_t const a1) {
 struct opt_usize_673 t0;
 t0 = mem_findScalarPos__anon_1363__1906(a0, (uintptr_t)0ul, a1);
 return t0;
}
zig_noinline struct slice_u8_49 mem_trim__anon_22429__11731(struct slice_u8_49 const a0, struct slice_u8_49 const a1) {
 struct slice_u8_49 const *t1;
 uintptr_t const *t4;
 uintptr_t t5;
 uintptr_t t6;
 uintptr_t t2;
 uintptr_t t3;
 uint32_t t7;
 uint32_t t8;
 struct slice_u8_49 t11;
 struct slice_u8_49 t0;
 uint8_t const *t12 zig_nonstring;
 struct opt_usize_673 t14;
 uint8_t const *t15 zig_nonstring;
 bool t9;
 bool t10;
 uint8_t t13;
 t0 = a0;
 t1 = (struct slice_u8_49 const *)&t0;
 t2 = (uintptr_t)0ul;
 t4 = &t1->len;
 t5 = (*t4);
 t3 = t5;
 zig_loop_17:
 t5 = t2;
 t6 = t3;
 t7 = zig_u32_bitCast_u32(t5, UINT8_C(32));
 t8 = zig_u32_bitCast_u32(t6, UINT8_C(32));
 t9 = t7 < t8;
 if (t9) {
  t6 = t2;
  t11 = (*t1);
  t12 = &t11.ptr[t6];
  t13 = (*t12);
  t14 = mem_findScalar__anon_2086__2440(a1, t13);
  t9 = !t14.is_null;
  t10 = t9;
  goto zig_block_2;
 }
 t10 = false;
 goto zig_block_2;

zig_block_2:;
 if (t10) {
  (void)0;
  t6 = t2;
  t6 = t6 + (uintptr_t)1ul;
  t2 = t6;
  goto zig_block_1;
 }
 goto zig_block_0;

zig_block_1:;
 goto zig_loop_17;

zig_block_0:;
 zig_loop_54:
 t6 = t3;
 t5 = t2;
 t8 = zig_u32_bitCast_u32(t6, UINT8_C(32));
 t7 = zig_u32_bitCast_u32(t5, UINT8_C(32));
 t10 = t8 > t7;
 if (t10) {
  t5 = t3;
  t5 = t5 - (uintptr_t)1ul;
  t11 = (*t1);
  t12 = &t11.ptr[t5];
  t13 = (*t12);
  t14 = mem_findScalar__anon_2086__2440(a1, t13);
  t10 = !t14.is_null;
  t9 = t10;
  goto zig_block_5;
 }
 t9 = false;
 goto zig_block_5;

zig_block_5:;
 if (t9) {
  (void)0;
  t5 = t3;
  t5 = t5 - (uintptr_t)1ul;
  t3 = t5;
  goto zig_block_4;
 }
 goto zig_block_3;

zig_block_4:;
 goto zig_loop_54;

zig_block_3:;
 t6 = t2;
 t5 = t3;
 t11 = (*t1);
 t15 = t11.ptr;
 t15 = (uint8_t const *)(((uintptr_t)t15) + (t6*sizeof(uint8_t)));
 t6 = t5 - t6;
 t11.ptr = t15;
 t11.len = t6;
 return t11;
}

/* MountPoints.find_locked: longest child path that prefixes `left`. */
struct child { struct slice_u8_49 path; struct child *next; };
__attribute__((noinline)) struct child *find_child(struct child *c, struct slice_u8_49 left)
{
  struct child *best = 0;
  for (; c; c = c->next) {
    if (c->path.len > left.len)
      continue;
    struct slice_u8_49 head = {left.ptr, c->path.len};
    if (mem_eql_generic(head, c->path) &&
        (left.len == c->path.len || left.ptr[c->path.len] == '/'))
      if (!best || c->path.len > best->path.len)
        best = c;
  }
  return best;
}

/* RomFs walk: the first directory entry whose name matches. */
struct entry { struct slice_u8_49 name; struct entry *next; };
__attribute__((noinline)) struct entry *find_entry(struct entry *e, struct slice_u8_49 part)
{
  while (e && !mem_eql_generic(e->name, part))
    e = e->next;
  return e;
}

/* An accumulator over a constant-trip, table-indexed loop: the shape whose
 * late unroll leaves `acc = 0 | x` and `index = #0`.  64-bit and 32-bit. */
__attribute__((noinline)) uint32_t xor_fold4(const uint8_t *a, const uint8_t *b, uintptr_t n)
{
  uintptr_t off[4] = {0, n - 4, (n / 8) * 4, n - 4 - (n / 8) * 4};
  uint32_t acc = 0;
  for (uintptr_t i = 0; i < 4; i++) {
    uint32_t x, y;
    memcpy(&x, a + off[i], 4);
    memcpy(&y, b + off[i], 4);
    acc |= x ^ y;
  }
  return acc;
}

__attribute__((noinline)) uint64_t sum64_4(const uint32_t *a, uintptr_t n)
{
  uintptr_t off[4] = {0, n - 1, n / 2, n / 3};
  uint64_t acc = 0;
  for (uintptr_t i = 0; i < 4; i++)
    acc += (uint64_t)a[off[i]] << (i * 8);
  return acc;
}

/* Identities on values the fold must keep exact. */
__attribute__((noinline)) uint32_t identities(uint32_t x, uint32_t z)
{
  uint32_t r = (x | z) + (z ^ x) - (x - z) + (x << z) + (x >> z);
  return r + (uint32_t)((int32_t)x >> z);
}

/* Return-block carriers (ra:return_sink): `ret = v; goto out; out: return ret;`
 * returns v at each arm.  Narrow carriers keep their extension, and a carrier
 * read elsewhere, volatile or address-taken keeps its writes. */
__attribute__((noinline)) int ret_goto(int x, const int *p)
{
  int ret;
  if (x < 0) { ret = -22; goto out; }
  if (!p) { ret = -14; goto out; }
  ret = *p + x;
out:
  return ret;
}

__attribute__((noinline)) unsigned char ret_uchar(int x)
{
  unsigned char r;
  if (x == 1) { r = 200; goto out; }
  if (x == 2) { r = (unsigned char)(x + 300); goto out; }
  if (x == 3) { r = (unsigned char)-x; goto out; }
  r = x > 7;
out:
  return r;
}

__attribute__((noinline)) signed char ret_schar(int x)
{
  signed char r;
  if (x == 1) { r = -100; goto out; }
  if (x == 2) { r = (signed char)(x * 100); goto out; }
  r = (signed char)x;
out:
  return r;
}

__attribute__((noinline)) short ret_short(int x)
{
  short r;
  if (x & 1) { r = -30000; goto out; }
  if (x & 2) { r = (short)(x << 15); goto out; }
  r = (short)(x * 3);
out:
  return r;
}

__attribute__((noinline)) int ret_reread(int x, int *seen)
{
  int r = 0;
  if (x < 0) { r = 1; goto out; }
  *seen = r + x;
  if (x == 5) { r = 9; goto out; }
  r = x * 2;
  *seen += r;
out:
  return r;
}

__attribute__((noinline)) int ret_volatile(int x)
{
  volatile int r;
  if (x < 0) { r = 1; goto out; }
  r = x + 1;
out:
  return r;
}

__attribute__((noinline)) static void bump(int *p) { *p += 40; }
__attribute__((noinline)) int ret_addr(int x)
{
  int r;
  int *q = &r;
  if (x < 0) { r = 1; goto out; }
  r = x;
  bump(q);
out:
  return r;
}

#include <stdio.h>
static uint8_t A[80], B[80];
int main(void)
{
  unsigned eq = 0, ne = 0;
  for (uintptr_t ao = 0; ao < 4; ++ao)
    for (uintptr_t bo = 0; bo < 4; ++bo)
      for (uintptr_t n = 0; n <= 40; ++n) {
        for (uintptr_t i = 0; i < n; ++i)
          A[ao + i] = B[bo + i] = (uint8_t)(i * 37 + n * 11 + 1);
        struct slice_u8_49 x = {A + ao, n}, y = {B + bo, n};
        eq += mem_eql_generic(x, y) + mem_eql_generic(x, x);
        struct slice_u8_49 shorter = {B + bo, n ? n - 1 : 1};
        ne += !mem_eql_generic(x, shorter) + !mem_eql_generic(shorter, x);
        for (uintptr_t i = 0; i < n; ++i) {
          B[bo + i] ^= 0x10;
          ne += !mem_eql_generic(x, y);
          B[bo + i] ^= 0x10;
        }
      }
  printf("eql: %u equal, %u differ\n", eq, ne);

  static const uint8_t usr[] = "usr", usrlib[] = "usr/lib", dev[] = "dev", us[] = "us";
  static const uint8_t path[] = "usr/lib/libc.so";
  struct child c3 = {{usrlib, 7}, 0}, c2 = {{dev, 3}, &c3}, c1 = {{usr, 3}, &c2}, c0 = {{us, 2}, &c1};
  struct slice_u8_49 left = {path, 15};
  struct child *best = find_child(&c0, left);
  printf("find_child: %.*s\n", (int)best->path.len, (const char *)best->path.ptr);
  struct slice_u8_49 none = {(const uint8_t *)"etc", 3};
  printf("find_child none: %d\n", find_child(&c0, none) == 0);

  static const uint8_t n0[] = "bin", n1[] = "lib", n2[] = "libc.so", n3[] = "sbin";
  struct entry e3 = {{n3, 4}, 0}, e2 = {{n2, 7}, &e3}, e1 = {{n1, 3}, &e2}, e0 = {{n0, 3}, &e1};
  struct slice_u8_49 p1 = {path + 8, 7}, p2 = {path + 4, 3}, p3 = {(const uint8_t *)"lib64", 5};
  printf("find_entry: %.*s %.*s %d\n", (int)find_entry(&e0, p1)->name.len,
         (const char *)find_entry(&e0, p1)->name.ptr, (int)find_entry(&e0, p2)->name.len,
         (const char *)find_entry(&e0, p2)->name.ptr, find_entry(&e0, p3) == 0);

  for (int i = 0; i < 80; i++)
    A[i] = B[i] = (uint8_t)(i * 7);
  B[9] ^= 1;
  printf("xor_fold4: %08x %08x\n", (unsigned)xor_fold4(A, B, 16), (unsigned)xor_fold4(A, A, 12));
  static uint32_t W[16];
  for (int i = 0; i < 16; i++)
    W[i] = 0x01010101u * (uint32_t)(i + 1);
  printf("sum64_4: %016llx\n", (unsigned long long)sum64_4(W, 16));
  printf("identities: %08x %08x\n", (unsigned)identities(0x80001234u, 0), (unsigned)identities(0x80001234u, 3));
  static const uint8_t ws[] = " /", raw[] = "/ /usr/lib/ /", nil[] = "", allws[] = "// /";
  struct slice_u8_49 set = {ws, 2}, empty_set = {ws, 0};
  struct slice_u8_49 tr = mem_trim__anon_22429__11731((struct slice_u8_49){raw, 13}, set);
  struct slice_u8_49 tr0 = mem_trim__anon_22429__11731((struct slice_u8_49){raw, 13}, empty_set);
  struct slice_u8_49 tr1 = mem_trim__anon_22429__11731((struct slice_u8_49){allws, 4}, set);
  struct slice_u8_49 tr2 = mem_trim__anon_22429__11731((struct slice_u8_49){nil, 0}, set);
  printf("trim: [%.*s] %u [%.*s] %u %u\n", (int)tr.len, (const char *)tr.ptr, (unsigned)(tr.ptr - raw),
         (int)tr0.len, (const char *)tr0.ptr, (unsigned)tr1.len, (unsigned)tr2.len);
  int one = 5, seen = 0;
  printf("ret_goto: %d %d %d\n", ret_goto(-1, &one), ret_goto(1, 0), ret_goto(2, &one));
  printf("ret_uchar: %d %d %d %d %d\n", ret_uchar(1), ret_uchar(2), ret_uchar(3), ret_uchar(8), ret_uchar(4));
  printf("ret_schar: %d %d %d %d\n", ret_schar(1), ret_schar(2), ret_schar(-7), ret_schar(300));
  printf("ret_short: %d %d %d\n", ret_short(1), ret_short(2), ret_short(12000));
  int rr0 = ret_reread(-3, &seen);
  int rr1 = ret_reread(5, &seen), seen1 = seen;
  int rr2 = ret_reread(6, &seen);
  printf("ret_reread: %d %d %d %d %d\n", rr0, rr1, seen1, rr2, seen);
  printf("ret_volatile: %d %d ret_addr: %d %d\n", ret_volatile(-1), ret_volatile(4), ret_addr(-1), ret_addr(2));
  return 0;
}
