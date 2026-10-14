/* The YasOS kernel's spin lock as the Zig C backend writes it: interrupt
 * masking in asm accessors, a weak compare-exchange spin loop with a WFE.
 * Under -minline-atomics every piece inlines into the callers -- no BL to a
 * lock helper and no __atomic_* call (test_machine_call_locks_inline). */
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t state; } SpinLock;

static uint32_t save_and_disable(void)
{
  uint32_t r;
  __asm volatile(" mrs %[ret], PRIMASK\n cpsid i" : [ret] "=r"(r)::"memory");
  return r;
}

static void restore(uint32_t m)
{
  __asm volatile(" msr PRIMASK, %[mask]" ::[mask] "r"(m) : "memory");
}

static void lock(SpinLock *l)
{
  uint32_t token = (*(volatile uint32_t *)0x4001f000 & 0xffu) + 1;
  for (;;)
  {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&l->state, &expected, token, true, 2 /* acquire */, 0))
      return;
    __asm volatile("wfe" ::: "memory");
  }
}

static void unlock(SpinLock *l)
{
  __atomic_store_n(&l->state, 0, 3 /* release */);
  __asm volatile("sev" ::: "memory");
}

static uint32_t lock_irqsave(SpinLock *l)
{
  uint32_t flags = save_and_disable();
  lock(l);
  return flags;
}

static void unlock_irqrestore(SpinLock *l, uint32_t flags)
{
  unlock(l);
  restore(flags);
}

SpinLock heap_lock, page_lock;
uint32_t heap_used, pages_used;

void heap_add(uint32_t n)
{
  uint32_t f = lock_irqsave(&heap_lock);
  heap_used += n;
  unlock_irqrestore(&heap_lock, f);
}

void pages_add(uint32_t n)
{
  uint32_t f = lock_irqsave(&page_lock);
  pages_used += n;
  unlock_irqrestore(&page_lock, f);
}

/* Lowering leaves these: a register clobber, and an "i" operand. */
void clobbers(void)
{
  __asm volatile("cpsid i" ::: "r4");
}

void immediate(void)
{
  __asm volatile("bkpt %0" ::"i"(3));
}
