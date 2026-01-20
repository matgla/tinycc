#include <stdint.h>
#include <stdio.h>

typedef union
{
  uint64_t u;
  struct
  {
    uint32_t lo;
    uint32_t hi;
  } w;
} u64_words;

static int fail_u64(const char *name, uint64_t got, uint64_t exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=0x%llx exp=0x%llx\n", name, (unsigned long long)got, (unsigned long long)exp);
    return 1;
  }
  return 0;
}

__attribute__((noinline)) static void mul32wide_u32(uint32_t a, uint32_t b, uint32_t *lo, uint32_t *hi)
{
  const uint32_t a0 = a & 0xFFFFu;
  const uint32_t a1 = a >> 16;
  const uint32_t b0 = b & 0xFFFFu;
  const uint32_t b1 = b >> 16;

  const uint32_t p0 = a0 * b0;
  const uint32_t p1 = a0 * b1;
  const uint32_t p2 = a1 * b0;
  const uint32_t p3 = a1 * b1;

  const uint32_t mid = (p0 >> 16) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);
  *lo = (p0 & 0xFFFFu) | (mid << 16);
  *hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
}

static inline uint32_t add32_c(uint32_t a, uint32_t b, uint32_t cin, uint32_t *cout)
{
  uint32_t s = a + b;
  uint32_t c = (s < a);
  uint32_t s2 = s + cin;
  c |= (s2 < s);
  *cout = c;
  return s2;
}

static inline void add64_shift32(uint32_t *w1, uint32_t *w2, uint32_t *w3, uint32_t lo, uint32_t hi)
{
  uint32_t c;
  *w1 = add32_c(*w1, lo, 0, &c);
  *w2 = add32_c(*w2, hi, c, &c);
  *w3 = add32_c(*w3, 0, c, &c);
}

static inline void add64_shift64(uint32_t *w2, uint32_t *w3, uint32_t lo, uint32_t hi)
{
  uint32_t c;
  *w2 = add32_c(*w2, lo, 0, &c);
  *w3 = add32_c(*w3, hi, c, &c);
}

/* Reference implementation: word-based extract/pack. */
__attribute__((noinline)) static void mul64wide_ref(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
  u64_words aa;
  u64_words bb;
  aa.u = a;
  bb.u = b;

  uint32_t a0 = aa.w.lo;
  uint32_t a1 = aa.w.hi;
  uint32_t b0 = bb.w.lo;
  uint32_t b1 = bb.w.hi;

  uint32_t p0_lo, p0_hi;
  uint32_t p1_lo, p1_hi;
  uint32_t p2_lo, p2_hi;
  uint32_t p3_lo, p3_hi;
  mul32wide_u32(a0, b0, &p0_lo, &p0_hi);
  mul32wide_u32(a0, b1, &p1_lo, &p1_hi);
  mul32wide_u32(a1, b0, &p2_lo, &p2_hi);
  mul32wide_u32(a1, b1, &p3_lo, &p3_hi);

  uint32_t w0 = p0_lo;
  uint32_t w1 = p0_hi;
  uint32_t w2 = 0;
  uint32_t w3 = 0;

  add64_shift32(&w1, &w2, &w3, p1_lo, p1_hi);
  add64_shift32(&w1, &w2, &w3, p2_lo, p2_hi);
  add64_shift64(&w2, &w3, p3_lo, p3_hi);

  u64_words out_lo;
  u64_words out_hi;
  out_lo.w.lo = w0;
  out_lo.w.hi = w1;
  out_hi.w.lo = w2;
  out_hi.w.hi = w3;
  *lo = out_lo.u;
  *hi = out_hi.u;
}

/* Original-style implementation: shifts and 64-bit pack. */
__attribute__((noinline)) static void mul64wide_orig(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
  uint32_t a0 = (uint32_t)a;
  uint32_t a1 = (uint32_t)(a >> 32);
  uint32_t b0 = (uint32_t)b;
  uint32_t b1 = (uint32_t)(b >> 32);

  uint32_t p0_lo, p0_hi;
  uint32_t p1_lo, p1_hi;
  uint32_t p2_lo, p2_hi;
  uint32_t p3_lo, p3_hi;
  mul32wide_u32(a0, b0, &p0_lo, &p0_hi);
  mul32wide_u32(a0, b1, &p1_lo, &p1_hi);
  mul32wide_u32(a1, b0, &p2_lo, &p2_hi);
  mul32wide_u32(a1, b1, &p3_lo, &p3_hi);

  uint32_t w0 = p0_lo;
  uint32_t w1 = p0_hi;
  uint32_t w2 = 0;
  uint32_t w3 = 0;

  add64_shift32(&w1, &w2, &w3, p1_lo, p1_hi);
  add64_shift32(&w1, &w2, &w3, p2_lo, p2_hi);
  add64_shift64(&w2, &w3, p3_lo, p3_hi);

  *lo = ((uint64_t)w1 << 32) | (uint64_t)w0;
  *hi = ((uint64_t)w3 << 32) | (uint64_t)w2;
}

static int check_one(uint64_t a, uint64_t b)
{
  uint64_t hi_ref, lo_ref;
  uint64_t hi_org, lo_org;

  mul64wide_ref(a, b, &hi_ref, &lo_ref);
  mul64wide_orig(a, b, &hi_org, &lo_org);

  int fails = 0;
  fails |= fail_u64("lo", lo_org, lo_ref);
  fails |= fail_u64("hi", hi_org, hi_ref);
  return fails;
}

int main(void)
{
  int fails = 0;

  /* Volatile seeds to avoid whole-program constant folding. */
  volatile uint64_t s0 = 0x0000000100000003ULL;
  volatile uint64_t s1 = 0x0000000200000007ULL;

  fails |= check_one((uint64_t)s0, (uint64_t)s1);
  fails |= check_one(0xFFFFFFFFFFFFFFFFULL, (uint64_t)s0);
  fails |= check_one(0x00000000FFFFFFFFULL, 0x00000000FFFFFFFFULL);
  fails |= check_one(0x1122334455667788ULL, 0xA1B2C3D4E5F60718ULL);
  fails |= check_one(0x0000000000000003ULL, 0x0000000000000003ULL);

  if (fails)
    return 1;
  printf("PASS\n");
  return 0;
}
