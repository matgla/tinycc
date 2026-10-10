/* ssa:ptr_store_dse deleted `*p = 1` across __builtin_apply, which calls a
 * function that reads *p. */
#include <stdio.h>
int g;
int *__attribute__((noinline)) gp(void) { return &g; }
void __attribute__((noinline)) show(int a) { printf("g=%d a=%d\n", g, a); }
void __attribute__((noinline)) fwd(int a)
{
  void *args = __builtin_apply_args();
  int *p = gp();
  *p = 1;
  __builtin_apply((void (*)())show, args, 16);
  *p = 2;
}
int main(void)
{
  fwd(7);
  printf("end g=%d\n", g);
  return 0;
}
