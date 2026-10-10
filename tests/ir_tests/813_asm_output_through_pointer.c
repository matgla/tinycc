/* An asm output that is an lvalue reached through a pointer -- "=m"(*p),
 * "=r"(*p), "=m"(s->f), "=r"(a[i]) -- named the pointer only in the
 * ASM_OUTPUT marker's deref destination, which dead-code passes do not count
 * as a use: `T0 <-- P0` was deleted and the store went through a register
 * nobody set (-O0) or one an input had just been loaded into (-O2), at every
 * level.  Found by the hazard sandwich tests (2026-10-06). */
#include <stdio.h>

struct S { int a, b; };

__attribute__((noinline)) int st_m(int *p, int v)
{
  int a = *p;
  __asm__ volatile("str %1, %0" : "=m"(*p) : "r"(v));
  return a * 1000 + *p;
}

__attribute__((noinline)) int st_r(int *p, int v)
{
  int a = *p;
  __asm__("mov %0, %1" : "=r"(*p) : "r"(v));
  return a * 1000 + *p;
}

__attribute__((noinline)) void st_field(struct S *s, int v)
{
  __asm__ volatile("str %1, %0" : "=m"(s->b) : "r"(v));
}

__attribute__((noinline)) void st_index(int *a, int i, int v)
{
  __asm__("add %0, %1, #1" : "=r"(a[i]) : "r"(v));
}

int main(void)
{
  int x = 1, y = 2, arr[4] = { 0, 0, 0, 0 };
  struct S s = { 3, 4 };
  printf("%d %d\n", st_m(&x, 5), x);
  printf("%d %d\n", st_r(&y, 6), y);
  st_field(&s, 7);
  st_index(arr, 2, 8);
  printf("%d %d %d %d %d %d\n", s.a, s.b, arr[0], arr[1], arr[2], arr[3]);
  return 0;
}
