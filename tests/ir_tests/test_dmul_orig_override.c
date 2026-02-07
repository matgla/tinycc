#include <stdint.h>
#include <stdio.h>

/* Pull in the original implementation as a normal object file.
 * The symbol __aeabi_dmul provided here should satisfy linking, so the
 * archive version will not be pulled in.
 */
#include "fixtures/dmul_orig.c"

static int fail_u64(const char *name, uint64_t got, uint64_t exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=0x%llx exp=0x%llx\n", name, (unsigned long long)got, (unsigned long long)exp);
    return 1;
  }
  return 0;
}

static uint64_t d_to_u(double d)
{
  union
  {
    double d;
    uint64_t u;
  } v;
  v.d = d;
  return v.u;
}

int main(void)
{
  int fails = 0;

  /* Known-good IEEE-754 encodings */
  fails |= fail_u64("dmul_3_3", d_to_u(__aeabi_dmul(3.0, 3.0)), 0x4022000000000000ULL);
  fails |= fail_u64("dmul_2_2", d_to_u(__aeabi_dmul(2.0, 2.0)), 0x4010000000000000ULL);
  fails |= fail_u64("dmul_3_2", d_to_u(__aeabi_dmul(3.0, 2.0)), 0x4018000000000000ULL);

  if (fails)
    return 1;
  printf("PASS\n");
  return 0;
}
