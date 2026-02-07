#include <stdio.h>

static long long g1 = 0x1122334455667788LL;
static long long g2 = -0x0011223344556677LL;

static long long load_through_ptr(const long long *p)
{
  return *p;
}

static void store_through_ptr(long long *p, long long v)
{
  *p = v;
}

static int check_s64(const char *name, long long got, long long exp)
{
  if (got != exp)
  {
    printf("FAIL %s got=%lld exp=%lld\n", name, got, exp);
    return 1;
  }
  return 0;
}

int main(void)
{
  printf("Testing signed long long loads/stores\n");

  long long local = 0;
  long long arr[3];
  arr[0] = g1;
  arr[1] = g2;
  arr[2] = -(1LL << 40);

  if (check_s64("g1", load_through_ptr(&g1), g1))
    return 1;
  if (check_s64("g2", load_through_ptr(&g2), g2))
    return 1;

  if (check_s64("arr0", load_through_ptr(&arr[0]), g1))
    return 1;
  if (check_s64("arr1", load_through_ptr(&arr[1]), g2))
    return 1;

  store_through_ptr(&local, arr[2]);
  if (check_s64("local", local, -(1LL << 40)))
    return 1;

  printf("PASS\n");
  return 0;
}
