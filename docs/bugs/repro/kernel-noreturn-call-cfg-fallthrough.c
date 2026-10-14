/* Reduced from the 2026-10-12 tcc kernel C (kernel.c): yasos_kheap_unlock ->
 * RecursiveRanked(.kheap).unlock_irqrestore -> SpinLock.unlock_irqrestore,
 * spelled exactly as Zig's C backend emits them. Only the panic body is
 * external (declared zig_noreturn, as in kernel.c:9057).
 *
 * Build (from the repo root):
 *   libs/tinycc/bin/armv8m-tcc -O2 -fno-pic -ffunction-sections -fdata-sections \
 *     -DYASLIBC_REAL_BOOL -mfloat-abi=hard -mfpu=fpv5-sp-d16 -fvisibility=hidden \
 *     -minline-atomics -include apps/zig/zig_c_prelude.h -I ~/.zvm/master/lib \
 *     -c libs/tinycc/docs/bugs/repro/kernel-noreturn-call-cfg-fallthrough.c -o /tmp/a.o
 * (Before the partial fix the output was byte-identical to the kernel ELF body
 * at 0x1000f380.) */
#define ZIG_TARGET_MAX_INT_ALIGNMENT 8
#include "zig.h"
struct slice_u8_49 {
 uint8_t const *ptr zig_nonstring;
 uintptr_t len;
};
struct opt_usize_673 {
 uintptr_t payload;
 bool is_null;
};
struct atomic_Value_28u32_29_1471 {
 uint32_t raw;
};
struct sync_spinlock_SpinLock_1431 {
 struct atomic_Value_28u32_29_1471 state;
};
struct sync_locks_RecursiveRanked_28_kheap_29_11575 {
 zig_align(32) struct sync_spinlock_SpinLock_1431 inner;
 uint32_t depth;
 uintptr_t outer_flags;
};
struct arr_42s115_u8_6042 { uint8_t array[43]; };
typedef struct sync_spinlock_SpinLock_1431 aligned__32_sync_spinlock_SpinLock_1431;
static struct arr_42s115_u8_6042 const __anon_11596 = {"recursive lock released without being held"};
zig_noreturn void lang_panic__struct_2290_panic__2474(struct slice_u8_49 a0, struct opt_usize_673 a1);
struct sync_locks_RecursiveRanked_28_kheap_29_11575 memory_heap_kheap_lock_lock__7691;

static void sync_spinlock_SpinLock_unlock__2004(struct sync_spinlock_SpinLock_1431 *const a0) {
 /* sync.spinlock.SpinLock.unlock */
 struct sync_spinlock_SpinLock_1431 *const *t1;
 struct sync_spinlock_SpinLock_1431 *t2;
 struct sync_spinlock_SpinLock_1431 *t0;
 struct atomic_Value_28u32_29_1471 *t3;
 struct atomic_Value_28u32_29_1471 *t4;
 struct atomic_Value_28u32_29_1471 *const *t5;
 uint32_t *t6;
 uint32_t t7;
 t0 = a0;
 t1 = (struct sync_spinlock_SpinLock_1431 *const *)&t0;
 /* 5:13 */
 t2 = (*t1);
 t3 = (struct atomic_Value_28u32_29_1471 *)&t2->state;
 /* 5:25 */
 /* inline:atomic.Value(u32).store */
 /* dbg_arg_inline:self */
 /* dbg_arg_inline:value */
 /* dbg_arg_inline:order */
 t4 = t3;
 t5 = (struct atomic_Value_28u32_29_1471 *const *)&t4;
 /* 2:34 */
 t3 = (*t5);
 t6 = (uint32_t *)&t3->raw;
 t7 = UINT32_C(0);
 zig_atomic_store((zig_atomic(uint32_t) *)t6, t7, zig_memory_order_release, u32, uint32_t);
 goto zig_block_0;

zig_block_0:;
 /* 7:13 */
 /* 7:56 */
 /* inline:sync.signal_event */
 /* 2:5 */
 __asm volatile("sev"::: "memory");
 goto zig_block_1;

zig_block_1:;
 return;
}
static void sync_spinlock_SpinLock_unlock_irqrestore__2006(struct sync_spinlock_SpinLock_1431 *const a0, uintptr_t const a1) {
 /* sync.spinlock.SpinLock.unlock_irqrestore */
 struct sync_spinlock_SpinLock_1431 *const *t1;
 struct sync_spinlock_SpinLock_1431 *t2;
 struct sync_spinlock_SpinLock_1431 *t0;
 t0 = a0;
 t1 = (struct sync_spinlock_SpinLock_1431 *const *)&t0;
 /* 2:20 */
 t2 = (*t1);
 /* 2:20 */
 sync_spinlock_SpinLock_unlock__2004(t2);
 /* 3:37 */
 /* inline:sync.restore_interrupts */
 /* dbg_arg_inline:primask */
 /* 2:5 */
 __asm volatile(" msr PRIMASK, %[mask]":: [mask]"r"(a1): "memory");
 goto zig_block_0;

zig_block_0:;
 return;
}
static void sync_locks_RecursiveRanked_28_kheap_29_unlock_irqrestore__8057(struct sync_locks_RecursiveRanked_28_kheap_29_11575 *const a0) {
 /* sync.locks.RecursiveRanked(.kheap).unlock_irqrestore */
 struct sync_locks_RecursiveRanked_28_kheap_29_11575 *const *t1;
 struct sync_locks_RecursiveRanked_28_kheap_29_11575 *t2;
 struct sync_locks_RecursiveRanked_28_kheap_29_11575 *t0;
 uint32_t *t3;
 uint32_t t4;
 uintptr_t *t6;
 uintptr_t t7;
 aligned__32_sync_spinlock_SpinLock_1431 *t8;
 struct sync_spinlock_SpinLock_1431 *t9;
 bool t5;
 t0 = a0;
 t1 = (struct sync_locks_RecursiveRanked_28_kheap_29_11575 *const *)&t0;
 /* 2:17 */
 /* 2:21 */
 t2 = (*t1);
 t3 = (uint32_t *)&t2->depth;
 t4 = (*t3);
 t5 = t4 == UINT32_C(0);
 if (t5) {
  /* 2:34 */
  lang_panic__struct_2290_panic__2474((struct slice_u8_49){((uint8_t const *)((struct arr_42s115_u8_6042 const *)&__anon_11596)),(uintptr_t)42ul}, (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul });
  zig_unreachable();
 }
 goto zig_block_0;

zig_block_0:;
 /* 3:17 */
 t2 = (*t1);
 t3 = (uint32_t *)&t2->depth;
 t4 = (*t3);
 /* 3:24 */
 t4 = t4 - UINT32_C(1);
 (*t3) = t4;
 /* 4:17 */
 /* 4:21 */
 t2 = (*t1);
 t3 = (uint32_t *)&t2->depth;
 t4 = (*t3);
 t5 = t4 != UINT32_C(0);
 if (t5) {
  /* 4:34 */
  return;
 }
 goto zig_block_1;

zig_block_1:;
 /* 6:31 */
 t2 = (*t1);
 t6 = (uintptr_t *)&t2->outer_flags;
 t7 = (*t6);
 /* dbg_var_val:flags */
 /* 7:17 */
 t2 = (*t1);
 t6 = (uintptr_t *)&t2->outer_flags;
 (*t6) = (uintptr_t)0ul;
 /* 8:17 */
 t2 = (*t1);
 t8 = (aligned__32_sync_spinlock_SpinLock_1431 *)&t2->inner;
 /* 8:41 */
 t9 = (struct sync_spinlock_SpinLock_1431 *)t8;
 /* 8:41 */
 sync_spinlock_SpinLock_unlock_irqrestore__2006(t9, t7);
 return;
}
void memory_heap_kheap_lock_yasos_kheap_unlock__7693(void) {
 sync_locks_RecursiveRanked_28_kheap_29_unlock_irqrestore__8057((((struct sync_locks_RecursiveRanked_28_kheap_29_11575 *)&memory_heap_kheap_lock_lock__7691)));
 return;
}

/* ---- Minimal plain-C variants (same flags) ---------------------------- */
struct S { unsigned a, depth, flags; };
struct S s_min;
__attribute__((noreturn)) void die(int);
void plain_call(int);
void sink(unsigned);
/* A: noreturn guard, as Zig emits it -> fixed: no reload, base in r0, LR pushed in the cold block only. */
unsigned guard_noreturn(void) { struct S *p = &s_min; if (p->depth == 0) { die(42); __builtin_unreachable(); } p->depth = p->depth - 1; return p->depth; }
/* B: control -- ordinary call then for(;;): the block ends in a JUMP. */
unsigned guard_selfloop(void) { struct S *p = &s_min; if (p->depth == 0) { plain_call(42); for (;;) ; } p->depth = p->depth - 1; return p->depth; }
/* D: no noreturn involved -- fixed: `subs; str; bne.w` (fusion across the store). */
void dec_store_test(void) { struct S *p = &s_min; unsigned v = p->depth - 1; p->depth = v; if (v != 0) return; unsigned f = p->flags; p->flags = 0; sink(f); }
/* C: the noreturn call laid out last -> fixed: LR pushed in the cold block only. */
unsigned guard_last(void) { struct S *p = &s_min; if (p->depth != 0) { p->depth = p->depth - 1; return p->depth; } die(42); }
