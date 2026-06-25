#include <stdio.h>

static int __attribute__((noinline)) func_a(int x)
{
  return x * 3 + 7;
}

static int __attribute__((noinline)) func_b(int x)
{
  return x * 5 - 3;
}

static int __attribute__((noinline)) func_c(int x)
{
  return (x << 2) + 1;
}

int bench_function_calls(void)
{
  int result = 0;
  int iterations = 1000;

  for (int n = 0; n < iterations; n++) {
    result = func_a(100);
    result = func_b(result);
    result = func_c(result);
    result = func_a(result);
    result = func_b(result);
  }

  return result;
}

int main(void)
{
    printf("function_calls: %d\n", bench_function_calls());
    return 0;
}
