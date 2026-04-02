#include <stdio.h>

static int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int bench_fibonacci(void)
{
  return fib(20);
}

int main(void)
{
    printf("fibonacci: %d\n", bench_fibonacci());
    return 0;
}
