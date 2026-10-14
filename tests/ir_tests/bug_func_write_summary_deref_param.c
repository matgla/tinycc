/* func_write_summary tracked `*pp` as the address pp+0, so a store through the
 * loaded pointer counted as a write of pp[0] and the caller's initialisation of
 * arr[0] was deleted.  A weak callee body is not trusted either.
 * Companion: bug_func_write_summary_deref_param+.c. */
#include <stdio.h>

__attribute__((noinline)) static void f2(int **pp) { **pp = 1; }

__attribute__((noinline)) int g3(int *y)
{
  int *arr[2];
  arr[0] = y;
  arr[1] = 0;
  f2(arr);
  return 0;
}

__attribute__((weak, noinline)) void fill(int *a) { a[0] = 1; a[1] = 1; }
extern int consume(int *a);

__attribute__((noinline)) int h(void)
{
  int arr[2];
  arr[0] = 5;
  arr[1] = 6;
  fill(arr);
  return consume(arr);
}

/* a plain direct write still forwards: the first store is dead */
__attribute__((noinline)) static void set0(int *a) { a[0] = 7; }
__attribute__((noinline)) int direct(void)
{
  int arr[2];
  arr[0] = 5;
  arr[1] = 6;
  set0(arr);
  return arr[0] * 10 + arr[1];
}

int main(void)
{
  int y = 0;
  g3(&y);
  printf("g3 %d h %d direct %d\n", y, h(), direct());
  return 0;
}
