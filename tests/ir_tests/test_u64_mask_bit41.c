#include <stdint.h>
#include <stdio.h>

static int fail(const char *name)
{
  printf("FAIL %s\n", name);
  return 1;
}

int main(void)
{
  int fails = 0;

  volatile uint64_t x0 = 0;
  volatile uint64_t x1 = 1ULL << 41;
  volatile uint64_t x2 = (1ULL << 41) | 0x1234ULL;

  volatile uint64_t mask = 1ULL << 41;

  if ((x0 & mask) != 0)
    fails |= fail("x0_mask");
  if ((x1 & mask) == 0)
    fails |= fail("x1_mask");
  if ((x2 & mask) == 0)
    fails |= fail("x2_mask");

  /* Also test a couple of nearby bits to catch off-by-one in the mask. */
  {
    volatile uint64_t m40 = 1ULL << 40;
    volatile uint64_t v40 = 1ULL << 40;
    if ((v40 & m40) == 0)
      fails |= fail("bit40");
    if ((v40 & mask) != 0)
      fails |= fail("bit40_vs_41");
  }

  if (fails)
    return 1;
  printf("PASS\n");
  return 0;
}
