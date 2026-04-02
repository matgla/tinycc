#include <stdio.h>

typedef int (*func_ptr_t)(int);

static int __attribute__((noinline)) func_ptr_add(int x)
{
  return x + 11;
}

static int __attribute__((noinline)) func_ptr_mul(int x)
{
  return x * 3;
}

static int __attribute__((noinline)) func_ptr_xor(int x)
{
  return x ^ 0x55AA;
}

int bench_indirect_calls(void)
{
  func_ptr_t ops[4] = {func_ptr_add, func_ptr_mul, func_ptr_xor, func_ptr_add};
  int value = 7;
  int iterations = 1000;

  for (int n = 0; n < iterations; n++) {
    value = ops[n & 3](value);
  }

  return value & 0x7FFFFFFF;
}

int main(void)
{
    printf("indirect_calls: %d\n", bench_indirect_calls());
    return 0;
}
