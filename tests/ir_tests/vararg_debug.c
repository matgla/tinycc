#include <stdio.h>

void test_vararg(const char *fmt, ...)
{
  __builtin_va_list ap;
  unsigned int fp_val;
  __asm__ volatile("mov %0, r7" : "=r"(fp_val));
  printf("FP = 0x%x\n", fp_val);
  printf("&fmt = 0x%x\n", (unsigned int)&fmt);
  printf("fmt value = 0x%x\n", (unsigned int)fmt);

  __builtin_va_start(ap, fmt);
  int a = __builtin_va_arg(ap, int);
  int b = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);

  printf("a = %d, b = %d\n", a, b);
}

int main(void)
{
  test_vararg("test", 10, 20);
  return 0;
}
