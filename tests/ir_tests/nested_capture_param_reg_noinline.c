/* nested_capture_param_reg_noinline.c — a noinline nested function capturing
 * REGISTER-passed parent parameters.
 *
 * Nested functions were registered as auto-inline candidates without checking
 * __attribute__((noinline)), and gen_function then cleared the captures'
 * addrtaken on the strength of that mark — so the parameters never reached a
 * frame slot, while the call it assumed away was still emitted.  add() read
 * both captures from [chain + 0] and returned 10 instead of 26.
 */
#include <stdio.h>

__attribute__((noinline)) static int reg_cap(int a, int b)
{
  __attribute__((noinline)) int add(int x) { return x + a + b; }
  return add(10);
}

int main(void)
{
  printf("reg_cap=%d\n", reg_cap(7, 9));
  return 0;
}
