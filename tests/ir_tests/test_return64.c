#include <stdint.h>
#include <stdio.h>

typedef union
{
  struct
  {
    unsigned int low, high;
  } s;
  long long ll;
} DWunion;

static long long test_return_local(long long a)
{
  DWunion u;
  u.ll = a;
  /* Just return the union - tests 64-bit return from local */
  return u.ll;
}

static int check_u64(const char *name, unsigned long long got, unsigned long long exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=0x%016llX exp=0x%016llX\n", name, got, exp);
    return 1;
  }
  printf("PASS %s\n", name);
  return 0;
}

int main(void)
{
  printf("Testing 64-bit return\n");

  if (check_u64("return_local_1", test_return_local(0x123456789ABCDEF0ULL), 0x123456789ABCDEF0ULL))
    return 1;
  if (check_u64("return_local_2", test_return_local(0x0000000080000000ULL), 0x0000000080000000ULL))
    return 1;

  printf("All tests passed!\n");
  return 0;
}
