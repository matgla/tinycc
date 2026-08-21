/* shift64_dead_half must be gated on the FINAL instruction stream.
 *
 * The pass marks a half of a 64-bit shift result dead when the shift has a
 * single use that reads only the other half, and codegen then skips emitting
 * it.  It used to run before tcc_ir_ssa_regalloc, so a later CSE that
 * redirected a second reader onto the same shift left the annotation claiming
 * a half was dead just as it acquired a live reader.  Codegen skipped that
 * half and the consumer read whatever the register happened to hold: a silent
 * wrong value, no crash, no warning.
 *
 * `wide()` is the exact shape.  The two `(x << 2)` expressions CSE into one
 * shift; the first reader takes only the high half, the second only the low
 * half.  Before the fix the low half was never computed and w2 came back as
 * an uninitialised register -- caught in the field by lib/fp/soft/ddiv.c,
 * where it corrupted every division whose dividend was a power of two.
 *
 * The other cases pin the annotation's real job, so a fix cannot just disable
 * it: shifts whose dead half genuinely is dead must still fold.
 */
#include <stdio.h>
#include <stdint.h>

/* Two readers of one shift: high half AND low half.  Nothing may be skipped. */
static uint64_t wide(uint64_t x)
{
  uint32_t hi = (uint32_t)((x << 2) >> 32);
  uint32_t lo = (uint32_t)(x << 2);
  return ((uint64_t)hi << 32) | lo;
}

/* Single reader taking only the high half: the low half IS dead here. */
static uint32_t only_hi(uint64_t x) { return (uint32_t)((x << 5) >> 32); }

/* Single reader taking only the low half. */
static uint32_t only_lo(uint64_t x) { return (uint32_t)(x << 5); }

/* Three readers spanning both halves, with the shift also feeding a compare. */
static uint64_t spread(uint64_t x)
{
  uint64_t s = x << 3;
  uint32_t hi = (uint32_t)(s >> 32);
  uint32_t lo = (uint32_t)s;
  return ((uint64_t)hi << 32) | lo | (s != 0);
}

int main(void)
{
  uint64_t v[] = { 0x0010000000000000ull, 1, 0xFFFFFFFFFFFFFFFFull,
                   0x00000000FFFFFFFFull, 0x123456789ABCDEF0ull };
  for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
  {
    uint64_t w = wide(v[i]), s = spread(v[i]);
    printf("wide=%08lx%08lx hi=%08lx lo=%08lx spread=%08lx%08lx\n",
           (unsigned long)(w >> 32), (unsigned long)(uint32_t)w,
           (unsigned long)only_hi(v[i]), (unsigned long)only_lo(v[i]),
           (unsigned long)(s >> 32), (unsigned long)(uint32_t)s);
  }
  printf("OK\n");
  return 0;
}
