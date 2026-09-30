/* Storing an array's address into memory keeps the array's initializer.
 *
 * dse took `src1 = Addr[array]` with a TEMP dest for "the TEMP is defined as
 * that address" -- but in a STORE_INDEXED the dest is the base pointer and in a
 * STORE through a pointer the address being written through: the array's
 * address was going into memory.  When that pointer was only ever written
 * through, the address was not marked read, and the stores initializing the
 * array were deleted (gcc.c-torture pr47538 once the double guard is gone). */
#include <stdio.h>

struct S
{
  int a, b, e, f;
  int *c;
  int d;
};

__attribute__((noinline)) static int sum(const struct S *y)
{
  return y->c[0] + y->c[1] * 10 + y->c[3] * 100 + y->d;
}

__attribute__((noinline)) static int through_field(void)
{
  struct S y;
  int c[4] = {1, 2, 3, 4};
  y.a = 10;
  y.b = 6;
  y.c = c; /* STORE_INDEXED off &y */
  y.d = 3;
  return sum(&y);
}

__attribute__((noinline)) static int through_pointer(void)
{
  struct S y;
  struct S *py = &y;
  int c[4] = {5, 6, 7, 8};
  py->a = 1;
  py->c = c; /* a STORE through a pointer */
  py->d = 9;
  return sum(py);
}

int main(void)
{
  printf("%d %d\n", through_field(), through_pointer());
  return 0;
}
