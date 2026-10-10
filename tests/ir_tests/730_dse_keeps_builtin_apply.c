/* dse deleted __builtin_apply when its result was unused: its opcode was not
 * on dse's list of ops with effects, so the call to set() vanished at -O1+. */
#include <stdio.h>
int g;
void set(int *q, int v) { *q = v; }
__attribute__((noinline)) int f(int *p, int v)
{
  void *args = __builtin_apply_args();
  int a = *p;
  __builtin_apply((void (*)())set, args, 16);
  int b = *p;
  return a * 100 + b;
}
__attribute__((noinline)) int f_used(int *p, int v)
{
  void *args = __builtin_apply_args();
  int a = *p;
  void *r = __builtin_apply((void (*)())set, args, 16);
  int b = *p;
  return a * 100 + b + (r == 0);
}
int main(void)
{
  g = 1;
  printf("%d\n", f(&g, 7));
  g = 2;
  printf("%d\n", f_used(&g, 9));
  return 0;
}
