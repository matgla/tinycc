/* Driver for abi_hfa_a.c / abi_hfa_b.c (see abi_hfa.h). */
#include <stdio.h>

int run_a(void);
int run_b(void);

int main(void)
{
  int a = run_a(), b = run_b();
  printf("a %x b %x\n", a, b);
  return a | b;
}
