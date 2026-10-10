/* The kernel's RankedMutex(.block).lock fast path, VERBATIM from the
 * tcc-kernel's kernel.c (SpinLock.lock -> lock_irqsave inlined into
 * RankedMutex(.block).lock), with the process/rank callees stubbed.
 * Compiling with the cross at the kernel's flags reproduces the on-device
 * fast path:
 *
 *   - the cmpxchg outcome (struct opt_u32.is_null) is stored to its stack
 *     slot and loaded straight back for the retry branch
 *     (strb rX,[sp,#N]; ldr rY,[sp,#N]; uxtb; cbnz), and
 *   - the mutex pointer is re-loaded through the pointer-to-parameter chain
 *     every iteration (ldr rZ,[sp,#M] inside the loop).
 *
 * Expected (gcc -O2, same source): the single-used opt is scalar-replaced,
 * the CAS result and the mutex pointer stay in registers through the loop.
 *
 * Build (kernel flags):
 *   armv8m-tcc -O2 -minline-atomics -c rankedmutex_lock_fastpath_2026-10-09.c -o /tmp/a.o -B<cross>
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -O2 -c rankedmutex_lock_fastpath_2026-10-09.c -o /tmp/b.o
 *   arm-none-eabi-objdump -d /tmp/a.o | sed -n '/<sync_mutex_RankedMutex_28_block_29_lock__7057>:/,/^$/p'
 */
#include <stdint.h>
#include <stdbool.h>
#define ZIG_TARGET_MAX_INT_ALIGNMENT 8
#include <zig.h>

#ifndef zig_cold
#define zig_cold __attribute__((cold))
#endif

struct atomic_Value_28u32_29_1471 { uint32_t raw; };
struct sync_spinlock_SpinLock_1431 { struct atomic_Value_28u32_29_1471 state; };
typedef struct sync_spinlock_SpinLock_1431 aligned__32_sync_spinlock_SpinLock_1431 __attribute__((aligned(32)));
struct opt_u32_2060 { uint32_t payload; uint8_t is_null; };
struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 { int placeholder; };

struct sync_mutex_RankedMutex_28_block_29_9483 {
    struct sync_spinlock_SpinLock_1431 guard;
    bool held;
    void *owner;
};

/* stubs for the kernel callees (bodies irrelevant to the fast path) */
static uint8_t cpu_Cpu_28source_cpu_Cpu_29_coreid__2329(void) { return 0; }
static struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 *sync_mutex_RankedMutex_28_block_29_current_process__7056(void) { return 0; }
static void sync_locks_enter_rank__anon_45162__16796(void) {}
static void sync_locks_exit_rank__anon_45162__16797(void) {}


struct slice_u8_49 { uint8_t const *ptr; uintptr_t len; };
struct arr_106s115_u8_4052 { uint8_t array[106]; };
static const struct arr_106s115_u8_4052 __anon_15960;
struct opt_usize_673 { uintptr_t payload; uint8_t is_null; };
__attribute__((noreturn, cold)) static void lang_panic__struct_2290_panic__2474(struct slice_u8_49 msg, struct opt_usize_673 ret_addr) { (void)msg; (void)ret_addr; __builtin_unreachable(); }

struct arr_119s115_u8_x { uint8_t array[119]; };
static const struct arr_119s115_u8_x __anon_15967;

struct arr_165s115_u8_x { uint8_t array[165]; };
static const struct arr_165s115_u8_x __anon_15976;
static uint8_t sync_preempt_preempt_disabled__2114(void);

struct arr_52s115_u8_x { uint8_t array[52]; };
static const struct arr_52s115_u8_x __anon_15983;
#define process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_block__4765(a, b) ((void)(a))

#define process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_is_bl__4766(p, b) (0)
#define irq_Irq_28irq_Irq_29_trigger__2364(id) ((void)(id))
#define process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_reeva__4770(p) ((void)(p))

/* ---- verbatim from kernel.c ---- */

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

static void sync_spinlock_SpinLock_lock__2003(struct sync_spinlock_SpinLock_1431 *const a0) {
 /* sync.spinlock.SpinLock.lock */
 struct sync_spinlock_SpinLock_1431 *const *t1;
 uint32_t t2;
 uint32_t t3;
 uint32_t t5;
 struct sync_spinlock_SpinLock_1431 *t6;
 struct sync_spinlock_SpinLock_1431 *t0;
 struct atomic_Value_28u32_29_1471 *t7;
 struct atomic_Value_28u32_29_1471 *t9;
 struct opt_u32_2060 t8;
 struct opt_u32_2060 t12;
 struct atomic_Value_28u32_29_1471 *const *t10;
 uint32_t *t11;
 uint8_t t4;
 bool t13;
 t0 = a0;
 t1 = (struct sync_spinlock_SpinLock_1431 *const *)&t0;
 /* 2:36 */
 /* inline:sync.spinlock.current_token */
 /* 3:31 */
 /* inline:sync.owner_id */
 /* 2:26 */
 t4 = cpu_Cpu_28source_cpu_Cpu_29_coreid__2329();
 /* 2:5 */
 t5 = zig_u32_intCast_u8(t4);
 t3 = t5;
 goto zig_block_1;

zig_block_1:;
 t3 = t3 & UINT32_C(2147483647);
 /* 3:49 */
 t3 = t3 + UINT32_C(1);
 /* 3:5 */
 t2 = t3;
 goto zig_block_0;

zig_block_0:;
 /* dbg_var_val:token */
 /* 7:13 */
 zig_loop_25:
 /* 12:20 */
 t6 = (*t1);
 t7 = (struct atomic_Value_28u32_29_1471 *)&t6->state;
 /* 12:38 */
 /* inline:atomic.Value(u32).cmpxchgWeak */
 /* dbg_arg_inline:self */
 /* dbg_arg_inline:expected_value */
 /* dbg_arg_inline:new_value */
 /* dbg_arg_inline:success_order */
 /* dbg_arg_inline:fail_order */
 t9 = t7;
 t10 = (struct atomic_Value_28u32_29_1471 *const *)&t9;
 /* 8:41 */
 t7 = (*t10);
 t11 = (uint32_t *)&t7->raw;
 t12.payload = UINT32_C(0);
 t12.is_null = zig_cmpxchg_weak((zig_atomic(uint32_t) *)t11, t12.payload, t2, zig_memory_order_acquire, zig_memory_order_relaxed, u32, uint32_t);
 /* 8:13 */
 t8 = t12;
 goto zig_block_4;

zig_block_4:;
 t13 = !t8.is_null;
 if (t13) {
  /* 16:32 */
  /* inline:sync.cpu_relax */
  /* 2:5 */
  __asm volatile("wfe"::: "memory");
  goto zig_block_5;

zig_block_5:;
  /* 17:9 */
  (void)0;
  goto zig_block_3;
 }
 goto zig_block_2;

zig_block_3:;
 goto zig_loop_25;

zig_block_2:;
 return;
}

static uintptr_t sync_spinlock_SpinLock_lock_irqsave__2005(struct sync_spinlock_SpinLock_1431 *const a0) {
 /* sync.spinlock.SpinLock.lock_irqsave */
 struct sync_spinlock_SpinLock_1431 *const *t1;
 uintptr_t t2;
 uintptr_t t3;
 struct sync_spinlock_SpinLock_1431 *t4;
 struct sync_spinlock_SpinLock_1431 *t0;
 t0 = a0;
 t1 = (struct sync_spinlock_SpinLock_1431 *const *)&t0;
 /* 2:60 */
 /* inline:sync.save_and_disable_interrupts */
 /* 2:5 */
 __asm volatile(" mrs %[ret], PRIMASK\n cpsid i": [ret]"=r"(t3):: "memory");
 /* 2:5 */
 t2 = t3;
 goto zig_block_0;

zig_block_0:;
 /* dbg_var_val:flags */
 /* 3:18 */
 t4 = (*t1);
 /* 3:18 */
 sync_spinlock_SpinLock_lock__2003(t4);
 /* 4:9 */
 return t2;
}

static zig_cold void sync_mutex_RankedMutex_28_block_29_lock__7057(struct sync_mutex_RankedMutex_28_block_29_9483 *const a0) {
 /* sync.mutex.RankedMutex(.block).lock */
 struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 t19;
 struct sync_mutex_RankedMutex_28_block_29_9483 *const *t1;
 struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 *t2;
 struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 *t12;
 struct sync_mutex_RankedMutex_28_block_29_9483 *t4;
 struct sync_mutex_RankedMutex_28_block_29_9483 *t0;
 aligned__32_sync_spinlock_SpinLock_1431 *t5;
 struct sync_spinlock_SpinLock_1431 *t6;
 uintptr_t t7;
 bool *t8;
 struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 **t10;
 uint32_t t13;
 struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 *t14;
 struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 *t15;
 struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 *t18;
 struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 *const *t16;
 void const *t17;
 bool t3;
 bool t9;
 bool t11;
 t0 = a0;
 t1 = (struct sync_mutex_RankedMutex_28_block_29_9483 *const *)&t0;
 /* 2:39 */
 t2 = sync_mutex_RankedMutex_28_block_29_current_process__7056();
 /* dbg_var_val:me */
 /* 3:29 */
 sync_locks_enter_rank__anon_45162__16796();
 zig_loop_10:
 /* 5:20 */
 /* 6:17 */
 /* 7:39 */
 t4 = (*t1);
 t5 = (aligned__32_sync_spinlock_SpinLock_1431 *)&t4->guard;
 /* 7:58 */
 t6 = (struct sync_spinlock_SpinLock_1431 *)t5;
 /* 7:58 */
 t7 = sync_spinlock_SpinLock_lock_irqsave__2005(t6);
 /* dbg_var_val:flags */
 /* 10:25 */
 /* 10:30 */
 t4 = (*t1);
 t8 = (bool *)&t4->held;
 t9 = (*t8);
 t9 = !t9;
 if (t9) {
  /* 11:29 */
  t4 = (*t1);
  t8 = (bool *)&t4->held;
  (*t8) = true;
  /* 12:29 */
  t4 = (*t1);
  t10 = (struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 **)&t4->owner;
  (*t10) = t2;
  /* 8:31 */
  t4 = (*t1);
  t5 = (aligned__32_sync_spinlock_SpinLock_1431 *)&t4->guard;
  /* 8:55 */
  t6 = (struct sync_spinlock_SpinLock_1431 *)t5;
  /* 8:55 */
  sync_spinlock_SpinLock_unlock_irqrestore__2006(t6, t7);
  t3 = false;
  goto zig_block_1;
 }
 goto zig_block_2;

zig_block_2:;
 /* 15:25 */
 t9 = t2 != NULL;
 if (t9) {
  /* 15:44 */
  t4 = (*t1);
  t10 = (struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 **)&t4->owner;
  t12 = (*t10);
  t9 = t12 == t2;
  t11 = t9;
  goto zig_block_4;
 }
 t11 = false;
 goto zig_block_4;

zig_block_4:;
 if (t11) {
  /* 16:25 */
  lang_panic__struct_2290_panic__2474((struct slice_u8_49){((uint8_t const *)((struct arr_106s115_u8_4052 const *)&__anon_15960)),(uintptr_t)106ul}, (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul });
  zig_unreachable();
 }
 goto zig_block_3;

zig_block_3:;
 /* 23:25 */
 /* 23:50 */
 /* inline:sync.in_handler_mode */
 /* 2:5 */
 __asm volatile("mrs %[ret], ipsr": [ret]"=r"(t13):);
 t9 = t13 != UINT32_C(0);
 /* 2:5 */
 t11 = t9;
 goto zig_block_6;

zig_block_6:;
 if (t11) {
  /* 24:25 */
  lang_panic__struct_2290_panic__2474((struct slice_u8_49){((uint8_t const *)((struct arr_119s115_u8_15966 const *)&__anon_15967)),(uintptr_t)119ul}, (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul });
  zig_unreachable();
 }
 goto zig_block_5;

zig_block_5:;
 /* 30:25 */
 /* 30:49 */
 t11 = sync_preempt_preempt_disabled__2114();
 if (t11) {
  /* 31:25 */
  lang_panic__struct_2290_panic__2474((struct slice_u8_49){((uint8_t const *)((struct arr_165s115_u8_4077 const *)&__anon_15976)),(uintptr_t)165ul}, (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul });
  zig_unreachable();
 }
 goto zig_block_7;

zig_block_7:;
 /* 33:25 */
 t11 = t2 != NULL;
 if (t11) {
  t14 = t2;
  t15 = t14;
  t16 = (struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 *const *)&t15;
  /* dbg_var_val:process */
  /* 36:41 */
  t14 = (*t16);
  t17 = (void const *)a0;
  /* 36:41 */
  process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_block__4765(t14, t17);
  /* 8:31 */
  t4 = (*t1);
  t5 = (aligned__32_sync_spinlock_SpinLock_1431 *)&t4->guard;
  /* 8:55 */
  t6 = (struct sync_spinlock_SpinLock_1431 *)t5;
  /* 8:55 */
  sync_spinlock_SpinLock_unlock_irqrestore__2006(t6, t7);
  t3 = true;
  goto zig_block_1;
 }
 goto zig_block_8;

zig_block_8:;
 /* 39:21 */
 lang_panic__struct_2290_panic__2474((struct slice_u8_49){((uint8_t const *)((struct arr_52s115_u8_12792 const *)&__anon_15983)),(uintptr_t)52ul}, (struct opt_usize_673){ .is_null = true, .payload = (uintptr_t)0xaaaaaaaaul });
 zig_unreachable();

zig_block_1:;
 /* dbg_var_val:blocked */
 /* 42:21 */
 t3 = !t3;
 if (t3) {
  /* 42:31 */
  return;
 }
 goto zig_block_9;

zig_block_9:;
 /* 48:35 */
 t14 = t2;
 t18 = t14;
 t16 = (struct process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_4595 *const *)&t18;
 /* dbg_var_val:process */
 zig_loop_130:
 /* 49:45 */
 t14 = (*t16);
 t19 = (*t14);
 t17 = (void const *)a0;
 /* 49:45 */
 t3 = process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_is_bl__4766(t19, t17);
 if (t3) {
  /* 50:36 */
  irq_Irq_28irq_Irq_29_trigger__2364(UINT8_C(1));
  /* 51:45 */
  t14 = (*t16);
  /* 51:45 */
  process_ProcessInterface_28process_ArmProcess_2cmemory_heap_process_memory_pool_ProcessMemoryPool_29_reeva__4770(t14);
  /* 52:17 */
  (void)0;
  goto zig_block_11;
 }
 goto zig_block_10;

zig_block_11:;
 goto zig_loop_130;

zig_block_10:;
 /* 53:13 */
 (void)0;
 goto zig_block_0;

zig_block_0:;
 goto zig_loop_10;
}



/* driver: keep the verbatim functions alive and exercised */
void ranked_mutex_lock(struct sync_mutex_RankedMutex_28_block_29_9483 *m);
void ranked_mutex_lock(struct sync_mutex_RankedMutex_28_block_29_9483 *m) {
    sync_mutex_RankedMutex_28_block_29_lock__7057(m);
}
