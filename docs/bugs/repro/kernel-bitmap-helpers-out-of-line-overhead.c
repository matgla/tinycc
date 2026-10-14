/* kernel-bitmap-helpers-out-of-line-overhead -- TmpMemoryPool(256).next_bit
 * (want_set=false instance) copied verbatim from the kernel.c of the
 * 2026-10-12 20:45 kernel_compare battery (only `static` dropped).  The unused
 * frame, the zig_ctz_u32 zero guard and the `mov;uxtb` on the clz result are
 * fixed; still shown: the ?usize returned through memory.  From the yasos.zig root:
 *   libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -fdata-sections \
 *     -DYASLIBC_REAL_BOOL -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fvisibility=hidden \
 *     -include apps/zig/zig_c_prelude.h -I ~/.zvm/master/lib \
 *     -c libs/tinycc/docs/bugs/repro/kernel-bitmap-helpers-out-of-line-overhead.c -o nb.o
 */
#define ZIG_TARGET_MAX_INT_ALIGNMENT 8
#include "zig.h"
struct mem_Allocator_VTable_4613;
struct mem_Allocator_4548 { void *ptr; struct mem_Allocator_VTable_4613 const *vtable; };
struct bit_set_Dynamic_6008 { uintptr_t bit_length; uintptr_t *masks; };
struct bit_set_DynamicManaged_6003 { struct mem_Allocator_4548 allocator; struct bit_set_Dynamic_6008 unmanaged; };
struct memory_heap_tmp_memory_pool_TmpMemoryPool_28256_29_29891 {
 uintptr_t memory_size; uintptr_t page_count; struct bit_set_DynamicManaged_6003 page_bitmap;
 uintptr_t start_address; uintptr_t peak_pages; uintptr_t used_pages; uintptr_t free_hint;
};
struct opt_usize_673 { uintptr_t payload; bool is_null; };
struct opt_usize_673 memory_heap_tmp_memory_pool_TmpMemoryPool_28256_29_next_bit__anon_41293__16331(struct memory_heap_tmp_memory_pool_TmpMemoryPool_28256_29_29891 *const a0, uintptr_t const a1, uintptr_t const a2);
struct opt_usize_673 memory_heap_tmp_memory_pool_TmpMemoryPool_28256_29_next_bit__anon_41293__16331(struct memory_heap_tmp_memory_pool_TmpMemoryPool_28256_29_29891 *const a0, uintptr_t const a1, uintptr_t const a2) {
 /* memory.heap.tmp_memory_pool.TmpMemoryPool(256).next_bit__anon_41293 */
 struct memory_heap_tmp_memory_pool_TmpMemoryPool_28256_29_29891 *const *t1;
 uint32_t t2;
 uint32_t t3;
 struct memory_heap_tmp_memory_pool_TmpMemoryPool_28256_29_29891 *t5;
 struct memory_heap_tmp_memory_pool_TmpMemoryPool_28256_29_29891 *t0;
 struct bit_set_DynamicManaged_6003 *t6;
 struct bit_set_Dynamic_6008 *t7;
 uintptr_t **t8;
 uintptr_t *t9;
 uintptr_t *t10;
 uintptr_t *const *t11;
 uintptr_t t13;
 uintptr_t t16;
 uintptr_t t12;
 uintptr_t t14;
 uintptr_t *t15;
 struct opt_usize_673 t19;
 struct opt_usize_673 t20;
 bool t4;
 uint8_t t17;
 uint8_t t18;
 t0 = a0;
 t1 = (struct memory_heap_tmp_memory_pool_TmpMemoryPool_28256_29_29891 *const *)&t0;
 /* 2:17 */
 t2 = zig_u32_bitCast_u32(a1, UINT8_C(32));
 t3 = zig_u32_bitCast_u32(a2, UINT8_C(32));
 t4 = t2 >= t3;
 if (t4) {
  /* 3:17 */
  return (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
 }
 goto zig_block_0;

zig_block_0:;
 /* 5:31 */
 t5 = (*t1);
 t6 = (struct bit_set_DynamicManaged_6003 *)&t5->page_bitmap;
 /* 5:43 */
 t7 = (struct bit_set_Dynamic_6008 *)&t6->unmanaged;
 /* 5:53 */
 t8 = (uintptr_t **)&t7->masks;
 t9 = (*t8);
 t10 = t9;
 t11 = (uintptr_t *const *)&t10;
 /* dbg_var_val:masks */
 /* 6:13 */
 /* 6:29 */
 t13 = a1 / (uintptr_t)32ul;
 t12 = t13;
 /* dbg_var_ptr:word */
 /* 7:13 */
 /* 7:28 */
 t13 = t12;
 /* 7:61 */
 t9 = (*t11);
 t15 = &t9[t13];
 t13 = (*t15);
 t13 = zig_not_u32(t13, UINT8_C(32));
 t14 = t13;
 /* dbg_var_ptr:bits */
 /* 8:13 */
 t13 = t14;
 /* 8:55 */
 t16 = a1 % (uintptr_t)32ul;
 /* 8:41 */
 t17 = zig_u8_intCast_u32(t16);
 /* 8:38 */
 t16 = zig_shlw_u32(UINTPTR_MAX, t17, UINT8_C(32));
 t16 = t13 & t16;
 t14 = t16;
 zig_loop_59:
 /* 9:20 */
 /* 10:21 */
 /* 11:21 */
 t16 = t14;
 t3 = zig_u32_bitCast_u32(t16, UINT8_C(32));
 t4 = t3 != UINT32_C(0);
 if (t4) {
  /* 12:21 */
  t16 = t12;
  /* 12:40 */
  t16 = t16 * (uintptr_t)32ul;
  t13 = t14;
  t18 = zig_ctz_u32(t13, UINT8_C(32));
  /* 12:52 */
  t13 = zig_u32_intCast_u8(t18);
  t13 = t16 + t13;
  /* dbg_var_val:index */
  /* 13:32 */
  t3 = zig_u32_bitCast_u32(t13, UINT8_C(32));
  t2 = zig_u32_bitCast_u32(a2, UINT8_C(32));
  t4 = t3 < t2;
  if (t4) {
   t20.is_null = false;
   t20.payload = t13;
   t19 = t20;
   goto zig_block_2;
  }
  t19 = (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
  goto zig_block_2;

zig_block_2:;
  /* 13:21 */
  return t19;
 }
 goto zig_block_1;

zig_block_1:;
 /* 15:17 */
 t16 = t12;
 /* 15:22 */
 t16 = t16 + (uintptr_t)1ul;
 t12 = t16;
 /* 16:21 */
 t16 = t12;
 /* 16:26 */
 t16 = t16 * (uintptr_t)32ul;
 t3 = zig_u32_bitCast_u32(t16, UINT8_C(32));
 t2 = zig_u32_bitCast_u32(a2, UINT8_C(32));
 t4 = t3 >= t2;
 if (t4) {
  /* 17:21 */
  return (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul };
 }
 goto zig_block_3;

zig_block_3:;
 /* 19:17 */
 /* 19:28 */
 t16 = t12;
 /* 19:61 */
 t9 = (*t11);
 t15 = &t9[t16];
 t16 = (*t15);
 t16 = zig_not_u32(t16, UINT8_C(32));
 t14 = t16;
 /* 20:13 */
 (void)0;
 goto zig_loop_59;
}
