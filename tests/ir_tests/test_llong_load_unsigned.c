#include <stdio.h>

static unsigned long long g1 = 0x1122334455667788ULL;
static unsigned long long g2 = 0x8000000000000001ULL;

static unsigned long long load_through_ptr(const unsigned long long *p)
{
  return *p;
}

static void store_through_ptr(unsigned long long *p, unsigned long long v)
{
  *p = v;
}

static int check_u64(const char *name, unsigned long long got, unsigned long long exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=%llu exp=%llu\n", name, got, exp);
    return 1;
  }
  return 0;
}

int main(void)
{
  printf("Testing unsigned long long loads/stores\n");

  unsigned long long local = 0;
  unsigned long long arr[3];
  arr[0] = g1;
  arr[1] = g2;
  arr[2] = 0xffffffffffffffffULL;

  if (check_u64("g1", load_through_ptr(&g1), g1))
    return 1;
  if (check_u64("g2", load_through_ptr(&g2), g2))
    return 1;

  if (check_u64("arr0", load_through_ptr(&arr[0]), g1))
    return 1;
  if (check_u64("arr1", load_through_ptr(&arr[1]), g2))
    return 1;

  store_through_ptr(&local, arr[2]);
  if (check_u64("local", local, 0xffffffffffffffffULL))
    return 1;

  printf("PASS\n");
  return 0;
}
