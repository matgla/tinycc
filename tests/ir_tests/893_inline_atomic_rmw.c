/* Read-modify-write atomics: __atomic_compare_exchange, __atomic_exchange and
   the fetch-and-op family at 1, 2 and 4 bytes.  Run as is, they call the
   runtime (armv8m_atomic.S); under -minline-atomics (test_qemu.py's inline
   atomics group) each is an LDREX/STREX loop in place (inline_atomic_rmw).
   Both must agree: signed narrow values compare with the zero-extended
   exclusive load, a failed compare-exchange writes the current value back
   and a successful one leaves *expected alone, op_fetch returns the new value,
   and a function that is nothing but the atomic still returns. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define RELAXED 0
#define ACQUIRE 2
#define RELEASE 3
#define ACQ_REL 4
#define SEQ_CST 5

static uint32_t w32;
static int16_t s16;
static int8_t s8;
static uint8_t u8;

/* The whole body is the atomic: no tail call to a helper that is not there. */
__attribute__((noinline)) uint32_t fetch_add_only(uint32_t *p, uint32_t v)
{
  return __atomic_fetch_add(p, v, SEQ_CST);
}

__attribute__((noinline)) bool cas_only(uint32_t *p, uint32_t *e, uint32_t d)
{
  return __atomic_compare_exchange(p, e, &d, false, ACQUIRE, RELAXED);
}

/* A run-time order is taken as seq_cst. */
__attribute__((noinline)) uint32_t xchg_with(uint32_t *p, uint32_t v, int order)
{
  uint32_t r;
  __atomic_exchange(p, &v, &r, order);
  return r;
}

/* The kernel's spin lock: weak compare-exchange in a loop. */
static uint32_t lock_word;
__attribute__((noinline)) void lock(uint32_t token)
{
  for (;;)
  {
    uint32_t expected = 0;
    if (__atomic_compare_exchange(&lock_word, &expected, &token, true, ACQUIRE, RELAXED))
      return;
  }
}

__attribute__((noinline)) void unlock(void)
{
  uint32_t zero = 0;
  __atomic_store(&lock_word, &zero, RELEASE);
}

int main(void)
{
  int fails = 0;

  /* 4 bytes: every op, every order */
  w32 = 10;
  uint32_t o1 = __atomic_fetch_add(&w32, 5, RELAXED);
  uint32_t o2 = __atomic_fetch_sub(&w32, 3, ACQUIRE);
  uint32_t o3 = __atomic_fetch_or(&w32, 0x100, RELEASE);
  uint32_t o4 = __atomic_fetch_xor(&w32, 0x101, ACQ_REL);
  uint32_t o5 = __atomic_fetch_and(&w32, 0xfe, SEQ_CST);
  uint32_t o6 = __atomic_fetch_nand(&w32, 0xf0, SEQ_CST);
  printf("fetch_op: %u %u %u %u %u %u -> %x\n", o1, o2, o3, o4, o5, o6, (unsigned)w32);

  w32 = 100;
  uint32_t n1 = __atomic_add_fetch(&w32, 7, RELAXED);
  uint32_t n2 = __atomic_sub_fetch(&w32, 2, SEQ_CST);
  uint32_t n3 = __atomic_or_fetch(&w32, 0x1000, ACQUIRE);
  uint32_t n4 = __atomic_xor_fetch(&w32, 0x1001, RELEASE);
  uint32_t n5 = __atomic_and_fetch(&w32, 0xff, ACQ_REL);
  uint32_t n6 = __atomic_nand_fetch(&w32, 0x0f, SEQ_CST);
  printf("op_fetch: %u %u %x %x %x %x -> %x\n", n1, n2, n3, n4, n5, n6, (unsigned)w32);

  /* compare-exchange: success leaves *expected, failure writes it back */
  w32 = 7;
  uint32_t e = 7, d = 9;
  bool ok1 = __atomic_compare_exchange(&w32, &e, &d, false, SEQ_CST, SEQ_CST);
  printf("cas ok: %d e=%u w=%u\n", ok1, e, w32);
  e = 3;
  d = 11;
  bool ok2 = __atomic_compare_exchange(&w32, &e, &d, true, ACQUIRE, RELAXED);
  printf("cas fail: %d e=%u w=%u\n", ok2, e, w32);

  /* signed narrow values whose sign bit is set */
  s16 = -1;
  int16_t e16 = -1, d16 = -32768;
  bool ok3 = __atomic_compare_exchange(&s16, &e16, &d16, false, SEQ_CST, SEQ_CST);
  e16 = 5;
  bool ok4 = __atomic_compare_exchange(&s16, &e16, &d16, false, SEQ_CST, SEQ_CST);
  printf("cas s16: %d %d e=%d s=%d\n", ok3, ok4, e16, s16);
  s8 = -128;
  int8_t e8 = -128, d8 = -1;
  bool ok5 = __atomic_compare_exchange(&s8, &e8, &d8, false, ACQ_REL, ACQUIRE);
  int8_t f8 = __atomic_fetch_add(&s8, 2, SEQ_CST);
  int8_t g8 = __atomic_add_fetch(&s8, -4, SEQ_CST);
  int16_t h16 = __atomic_nand_fetch(&s16, 0x7fff, SEQ_CST);
  printf("s8: %d f=%d g=%d s=%d h16=%d\n", ok5, f8, g8, s8, h16);

  /* 1-byte exchange and wraparound */
  u8 = 250;
  uint8_t v8 = 3, r8;
  __atomic_exchange(&u8, &v8, &r8, ACQUIRE);
  uint8_t w8 = __atomic_add_fetch(&u8, 255, RELAXED);
  printf("u8: r=%u w=%u u=%u\n", r8, w8, u8);

  /* out-of-line forms */
  w32 = 40;
  uint32_t a = fetch_add_only(&w32, 2);
  uint32_t ex = 42;
  bool c1 = cas_only(&w32, &ex, 50);
  ex = 1;
  bool c2 = cas_only(&w32, &ex, 60);
  uint32_t x1 = xchg_with(&w32, 70, 9);
  printf("calls: a=%u c1=%d c2=%d ex=%u x1=%u w=%u\n", a, c1, c2, ex, x1, w32);

  lock(5);
  printf("locked: %u\n", lock_word);
  unlock();
  lock(6);
  printf("relocked: %u\n", lock_word);
  unlock();

  if (w32 != 70 || lock_word != 0)
    fails++;
  return fails;
}
