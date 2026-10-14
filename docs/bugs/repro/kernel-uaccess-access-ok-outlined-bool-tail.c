/* kernel-uaccess-access-ok-outlined-bool-tail: uaccess.access_ok (write
 * variant) and its syscall-dispatch caller, spelled exactly as Zig's C backend
 * emits them in the 2026-10-12 kernel.c (names shortened, bodies verbatim).
 *
 *   libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -fdata-sections \
 *     -DYASLIBC_REAL_BOOL -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fvisibility=hidden \
 *     -include apps/zig/zig_c_prelude.h -I ~/.zvm/master/lib \
 *     -c libs/tinycc/docs/bugs/repro/kernel-uaccess-access-ok-outlined-bool-tail.c -o /tmp/r.o
 *   arm-none-eabi-objdump -d /tmp/r.o
 *
 * Expected (LLVM / gcc): access_ok inlined into dispatch(); its result tested
 * straight off the range loop's compares.  tcc: access_ok stays out of line
 * (bl + push/pop), and its tail rematerialises the already-boolean result
 * (mov; uxtb; cbz; movs #1; b; movs #0; uxtb) after the loop has set it.
 */
#define ZIG_TARGET_SOFT_COMPILER_RT_F80_ABI
#define ZIG_TARGET_SOFT_COMPILER_RT_F128_ABI
#define ZIG_TARGET_MAX_INT_ALIGNMENT 8
#include "zig.h"

struct uaccess_Region { uintptr_t start; uintptr_t end; };
struct arr_4_uaccess_Region { struct uaccess_Region array[4]; };
struct slice_uaccess_Region { struct uaccess_Region const *ptr; uintptr_t len; };
struct slice_uaccess_Region_mut { struct uaccess_Region *ptr; uintptr_t len; };
struct errunion_usize { uintptr_t payload; uint16_t error; };
struct tuple_2_usize_u1 { uintptr_t f0; uint8_t f1; };
struct tuple_warn { uintptr_t f1; uintptr_t f2; };
enum { zig_error_Overflow = 5u };

uintptr_t uaccess_writable_count;
struct arr_4_uaccess_Region uaccess_writable;
bool uaccess_initialized;

static void debug_assert(bool const a0) {
 bool t0;
 t0 = !a0;
 if (t0) {
  zig_unreachable();
 }
 goto zig_block_0;

zig_block_0:;
 return;
}

static struct errunion_usize math_add(uintptr_t const a0, uintptr_t const a1) {
 struct tuple_2_usize_u1 t0;
 struct tuple_2_usize_u1 t1;
 struct tuple_2_usize_u1 const *t2;
 uint8_t const *t3 zig_nonstring;
 uintptr_t const *t6;
 uintptr_t t7;
 struct errunion_usize t8;
 uint8_t t4;
 bool t5;
 t0.f1 = zig_addo_u32((uint32_t *)&t0.f0, a0, a1, UINT8_C(32));
 t1 = t0;
 t2 = (struct tuple_2_usize_u1 const *)&t1;
 t3 = (uint8_t const *)&t2->f1;
 t4 = (*t3);
 t5 = t4 != UINT8_C(0);
 if (t5) {
  return (struct errunion_usize){ .error = zig_error_Overflow, .payload = (uintptr_t)0xaaaaaaaaul };
 }
 goto zig_block_0;

zig_block_0:;
 t6 = (uintptr_t const *)&t2->f0;
 t7 = (*t6);
 t8.payload = t7;
 t8.error = UINT16_C(0);
 return t8;
}

static bool uaccess_range_ok(struct slice_uaccess_Region const a0, uintptr_t const a1, uintptr_t const a2) {
 uint32_t t0;
 uint32_t t7;
 uintptr_t t2;
 uintptr_t t4;
 uintptr_t t6;
 uintptr_t t12;
 uintptr_t t5;
 struct errunion_usize t3;
 struct uaccess_Region t8;
 struct uaccess_Region t9;
 struct uaccess_Region const *t10;
 uintptr_t const *t11;
 bool t1;
 bool t13;
 t0 = zig_u32_bitCast_u32(a2, UINT8_C(32));
 t1 = t0 == UINT32_C(0);
 if (t1) {
  return true;
 }
 goto zig_block_0;

zig_block_0:;
 t3 = math_add(a1, a2);
 t1 = t3.error == UINT16_C(0);
 if (t1) {
  t4 = t3.payload;
  t2 = t4;
  goto zig_block_1;
 }
 return false;

zig_block_1:;
 t5 = (uintptr_t)0ul;
 t4 = a0.len;
 zig_loop_30:
 t6 = t5;
 t0 = zig_u32_bitCast_u32(t6, UINT8_C(32));
 t7 = zig_u32_bitCast_u32(t4, UINT8_C(32));
 t1 = t0 < t7;
 if (t1) {
  t8 = a0.ptr[t6];
  t9 = t8;
  t10 = (struct uaccess_Region const *)&t9;
  t11 = (uintptr_t const *)&t10->start;
  t12 = (*t11);
  t7 = zig_u32_bitCast_u32(a1, UINT8_C(32));
  t0 = zig_u32_bitCast_u32(t12, UINT8_C(32));
  t1 = t7 >= t0;
  if (t1) {
   t11 = (uintptr_t const *)&t10->end;
   t12 = (*t11);
   t0 = zig_u32_bitCast_u32(t2, UINT8_C(32));
   t7 = zig_u32_bitCast_u32(t12, UINT8_C(32));
   t1 = t0 <= t7;
   t13 = t1;
   goto zig_block_5;
  }
  t13 = false;
  goto zig_block_5;

zig_block_5:;
  if (t13) {
   return true;
  }
  goto zig_block_4;

zig_block_4:;
  (void)0;
  goto zig_block_3;
 }
 goto zig_block_2;

zig_block_3:;
 t6 = t6 + (uintptr_t)1ul;
 t5 = t6;
 goto zig_loop_30;

zig_block_2:;
 return false;
}

static bool uaccess_permitted(uintptr_t const a0, uintptr_t const a1) {
 uintptr_t t1;
 struct slice_uaccess_Region_mut t2;
 struct slice_uaccess_Region t3;
 bool t0;
 t0 = (*(((bool *)&uaccess_initialized)));
 debug_assert(t0);
 t1 = (*(((uintptr_t *)&uaccess_writable_count)));
 t1 = t1 - (uintptr_t)0ul;
 t2.ptr = ((struct uaccess_Region *)((struct arr_4_uaccess_Region *)&uaccess_writable));
 t2.len = t1;
 t3.ptr = (struct uaccess_Region const *)t2.ptr;
 t3.len = t2.len;
 t0 = uaccess_range_ok(t3, a0, a1);
 if (t0) {
  return true;
 }
 goto zig_block_0;

zig_block_0:;
 return false;
}

/* log.scoped(.uaccess).warn -> log.log: compiled out (empty body). */
static void log_log_warn(struct tuple_warn const a0) {
 (void)a0;
 return;
}
static void log_uaccess_warn(struct tuple_warn const a0) {
 log_log_warn(a0);
 return;
}

static bool uaccess_access_ok(uintptr_t const a0, uintptr_t const a1) {
 struct tuple_warn t2;
 bool t0;
 bool t1;
 t0 = uaccess_permitted(a0, a1);
 t1 = !t0;
 if (t1) {
  t2.f1 = a1;
  t2.f2 = a0;
  log_uaccess_warn(t2);
  goto zig_block_0;
 }
 goto zig_block_0;

zig_block_0:;
 return t0;
}

/* Shape of interrupts.system_call._irq_svcall's first check. */
bool caller_is_untrusted(void);
intptr_t dispatch(uint32_t const a0, void volatile *const a2) {
 uintptr_t t3;
 bool t0;
 bool t2;
 bool t4;
 t0 = caller_is_untrusted();
 if (t0) {
  t3 = (uintptr_t)a2;
  t4 = uaccess_access_ok(t3, (uintptr_t)8ul);
  t4 = !t4;
  t2 = t4;
  goto zig_block_2;
 }
 t2 = false;
 goto zig_block_2;

zig_block_2:;
 if (t2) {
  return -(intptr_t)1;
 }
 return (intptr_t)a0;
}

/* A second, unrelated caller (uaccess.check), as in the kernel. */
uint16_t check(uintptr_t const a0, uintptr_t const a1) {
 bool t0;
 t0 = uaccess_access_ok(a0, a1);
 t0 = !t0;
 if (t0) {
  return 23;
 }
 return 0;
}
