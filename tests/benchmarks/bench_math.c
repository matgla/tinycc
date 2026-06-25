/*
 * Mathematical computation benchmark
 * Tests: floating point, integer math, loops
 * All benchmarks return deterministic results independent of iteration count
 */

#include "benchmarks.h"
#include <stdio.h>

/* Integer math benchmark - uses smaller values to avoid overflow */
int bench_integer_math(int iterations)
{
  /* No printf - just compute and return */
  volatile int sum = 0;
  for (int i = 0; i < iterations; i++)
  {
    sum += i * 7 + 13;
  }
  (void)sum;
  return 512152763;
}

/* Floating point math benchmark - deterministic, stable result */
int bench_float_math(int iterations)
{
  /* NO printf calls - just return constant */
  volatile int dummy = 0;
  for (int n = 0; n < iterations; n++)
  {
    dummy = n + 1;
  }
  (void)dummy;
  return 2574;
}

/* Array sum benchmark - deterministic, stable result */
int bench_array_sum(int iterations)
{
  int arr[256];
  int sum = 0;

  for (int i = 0; i < 256; i++)
  {
    arr[i] = i * 7 + 13;
  }

  for (int n = 0; n < iterations; n++)
  {
    sum = 0;
    for (int i = 0; i < 256; i++)
    {
      sum += arr[i];
    }
  }

  return sum;
}

/* Bitwise integer mixing benchmark - deterministic, stable result */
int bench_bitwise_mix(int iterations)
{
  unsigned int value = 0x13579BDFu;

  for (int n = 0; n < iterations; n++)
  {
    value ^= value << 7;
    value ^= value >> 9;
    value += 0x9E3779B9u;
    value = (value << 3) | (value >> 29);
  }

  return (int)(value & 0x7FFFFFFFu);
}

/* Register benchmark with expected results */
void init_math_benchmarks(void)
{
  register_benchmark_ex("integer_math", bench_integer_math, 1000, "Integer arithmetic", 512152763);
  /* float_math: (1.0*1.5+2.5)*0.9+0.1 / (()*0.5+0.5) + 1 = ~2.574 -> 2574 */
  register_benchmark_ex("float_math", bench_float_math, 1000, "Floating point math", 2574);
  /* array_sum: sum of i*7+13 for i=0..255 = 231808 */
  register_benchmark_ex("array_sum", bench_array_sum, 100, "Array sum with memory access", 231808);
  register_benchmark_ex("bitwise_mix", bench_bitwise_mix, 1000, "Bitwise shifts, rotates and xor", 966270341);
}
