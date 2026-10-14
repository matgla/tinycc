// pico-sdk's software spinlock release on RP2350, through tcc -O2 in the
// YasOS kernel: `uint32_t zero = 0; asm("stlb %0, [%1]" :: "r"(zero),
// "r"(lock))`.  The optimizer propagated 0 into the asm's input marker and
// deleted the local's store and its frame slot, but inline-asm lowering
// still loaded the operand from that slot -- below SP once the frame was
// gone -- and the kernel bus-faulted unlocking in irq_set_exclusive_handler.
// scribble() leaves 0xAA.. below SP so a read of the dead slot shows.
#include <stdio.h>

typedef volatile unsigned char spin_lock_t;
spin_lock_t locks[4] = {1, 1, 1, 1};
typedef void (*handler_t)(void);
handler_t table[32];
unsigned word;

static void handler(void) {}

__attribute__((noinline)) void scribble(void)
{
  volatile unsigned buf[64];
  for (int i = 0; i < 64; i++)
    buf[i] = 0xAAAAAAAAu;
}

static inline __attribute__((always_inline)) void release(spin_lock_t *lock)
{
  ({
    unsigned zero = 0;
    __asm volatile("strb %0, [%1]\n" : : "r"(zero), "r"(lock) : "memory");
  });
}

static inline __attribute__((always_inline)) void store_word(unsigned *p, unsigned seed)
{
  unsigned v = 0x1234;
  unsigned copy = seed; /* copy-propagated into the marker: another vreg */
  __asm volatile("str %0, [%1]\n\tadd %0, %0, %2\n\tstr %0, [%1]" : : "r"(v), "r"(p), "r"(copy) : "memory");
}

static void set_and_unlock(unsigned num, handler_t f)
{
  table[num] = f;
  __asm volatile("" ::: "memory");
  release(&locks[2]);
}

__attribute__((noinline)) void install(unsigned num, handler_t f) { set_and_unlock(num, f); }
__attribute__((noinline)) void put(unsigned seed) { store_word(&word, seed); }

int main(void)
{
  scribble();
  install(3, handler);
  scribble();
  put(0x10);
  printf("lock=%d installed=%d word=%x\n", locks[2], table[3] == handler, word);
  return 0;
}
