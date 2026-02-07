#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) static void split_u64_params(uint64_t a, uint64_t b, uint32_t out[4])
{
  out[0] = (uint32_t)a;
  out[1] = (uint32_t)(a >> 32);
  out[2] = (uint32_t)b;
  out[3] = (uint32_t)(b >> 32);
}

static int check_u32(const char *name, uint32_t got, uint32_t exp)
{
  if (got == exp)
    return 0;
  printf("FAIL %s got=0x%08x exp=0x%08x\n", name, (unsigned)got, (unsigned)exp);
  return 1;
}

int main(void)
{
  uint32_t out[4] = {0, 0, 0, 0};
  split_u64_params(0x1122334455667788ULL, 0x99aabbccddeeff00ULL, out);

  if (check_u32("a.lo", out[0], 0x55667788u))
    return 1;
  if (check_u32("a.hi", out[1], 0x11223344u))
    return 1;
  if (check_u32("b.lo", out[2], 0xddeeff00u))
    return 1;
  if (check_u32("b.hi", out[3], 0x99aabbccu))
    return 1;

  printf("PASS\n");
  return 0;
}
