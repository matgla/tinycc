/* __atomic_load / __atomic_store of 1-, 2- and 4-byte objects are inline
   (parse_atomic): an LDR/STR, with a DMB for the stronger orders -- no call,
   so no runtime helper is needed (the ARMv8-M libtcc1 has none).  Every
   order, a run-time order, the generic pointer forms, and a value that must
   come through the access unchanged at each width. */
#include <stdio.h>
#include <stdint.h>

#define RELAXED 0
#define ACQUIRE 2
#define RELEASE 3
#define SEQ_CST 5

static uint8_t b8;
static uint16_t h16;
static uint32_t w32;
static int32_t s32 = -5;

__attribute__((noinline)) uint32_t load_with(uint32_t *p, int order)
{
  uint32_t r;
  __atomic_load(p, &r, order);
  return r;
}

__attribute__((noinline)) void store_with(uint32_t *p, uint32_t v, int order)
{
  __atomic_store(p, &v, order);
}

int main(void)
{
  uint8_t v8 = 0xA5;
  uint16_t v16 = 0xBEEF;
  uint32_t v32 = 0xDEADBEEF, r32;
  uint8_t r8;
  uint16_t r16;
  int32_t rs;

  __atomic_store(&b8, &v8, RELAXED);
  __atomic_store(&h16, &v16, RELEASE);
  __atomic_store(&w32, &v32, SEQ_CST);
  __atomic_load(&b8, &r8, ACQUIRE);
  __atomic_load(&h16, &r16, SEQ_CST);
  __atomic_load(&w32, &r32, RELAXED);
  __atomic_load(&s32, &rs, ACQUIRE);
  printf("b8=%x h16=%x w32=%x s32=%d\n", r8, r16, (unsigned)r32, (int)rs);

  store_with(&w32, 1234, RELAXED);
  unsigned a = load_with(&w32, ACQUIRE);
  store_with(&w32, 5678, SEQ_CST);
  unsigned b = load_with(&w32, SEQ_CST);
  store_with(&w32, 42, 1 /* consume: not valid for a store, taken as seq_cst */);
  unsigned c = load_with(&w32, 4 /* acq_rel: not valid for a load */);
  printf("runtime orders: a=%u b=%u c=%u\n", a, b, c);
  return 0;
}
