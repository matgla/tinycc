#include <stdint.h>
#include <stdio.h>

static int fail_u64(const char *name, uint64_t got, uint64_t exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=0x%llx exp=0x%llx\n", name, (unsigned long long)got, (unsigned long long)exp);
    return 1;
  }
  return 0;
}

static int fail_u32(const char *name, uint32_t got, uint32_t exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=0x%lx exp=0x%lx\n", name, (unsigned long)got, (unsigned long)exp);
    return 1;
  }
  return 0;
}

int main(void)
{
  int fails = 0;

  /* Volatile to prevent constant folding: we want to test runtime codegen. */
  volatile uint64_t a = 0x1122334455667788ULL;
  volatile uint64_t b = 0xA1B2C3D4E5F60718ULL;

  /* Force actual memory reads of volatile values through addressable storage.
   * The IR optimizer currently does not model C volatility, so relying on
   * plain `volatile` locals alone is not robust under -O1.
   */
  volatile uint64_t *ap = &a;
  volatile uint64_t *bp = &b;
  const uint64_t aval = *ap;
  const uint64_t bval = *bp;

  /* Immediate shift-by-32 edge cases */
  fails |= fail_u64("shr32_u64", (uint64_t)(aval >> 32), 0x0000000011223344ULL);
  fails |= fail_u64("shl32_u64", (uint64_t)(aval << 32), 0x5566778800000000ULL);

  /* Ensure truncation happens after the 64-bit shift */
  fails |= fail_u32("shr32_to_u32", (uint32_t)(aval >> 32), 0x11223344u);

  /* Pack/unpack patterns seen in the FP runtime */
  {
    volatile uint32_t lo = (uint32_t)aval;
    volatile uint32_t hi = (uint32_t)(aval >> 32);
    uint64_t roundtrip = ((uint64_t)hi << 32) | (uint64_t)lo;
    fails |= fail_u64("roundtrip_pack", roundtrip, (uint64_t)aval);
  }

  /* Mixed expression to discourage over-simplification */
  {
    uint32_t bh = (uint32_t)(bval >> 32);
    uint32_t bl = (uint32_t)bval;
    uint64_t x = ((uint64_t)bh << 32) | bl;
    fails |= fail_u64("roundtrip_pack_2", x, (uint64_t)bval);
  }

  if (fails)
  {
    return 1;
  }
  printf("PASS\n");
  return 0;
}
