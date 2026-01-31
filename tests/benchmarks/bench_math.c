/*
 * Mathematical computation benchmark
 * Tests: floating point, integer math, loops
 * All benchmarks return deterministic results independent of iteration count
 */

#include "benchmarks.h"

/* Integer math benchmark - uses smaller values to avoid overflow */
int bench_integer_math(int iterations)
{
  int result = 0;

  for (int n = 0; n < iterations; n++)
  {
    /* Use smaller values to avoid 32-bit overflow */
    int a = 1234;
    int b = 567;

    result = a * b + (a >> 3) - (b << 2);
    result += (result * 31) >> 5;
    result ^= (result << 13);
    result += 42;
  }

  /* Keep result in valid positive range */
  return result & 0x7FFFFFFF;
}

/* Floating point math benchmark - deterministic, stable result */
int bench_float_math(int iterations)
{
  float result = 0.0f;

  for (int n = 0; n < iterations; n++)
  {
    float a = 1.5f;
    float b = 2.5f;
    float r = 1.0f;

    r = r * a + b;
    r = r * 0.9f + 0.1f;
    r = r / (r * 0.5f + 0.5f) + 1.0f;
    result = r;
  }

  return (int)(result * 1000);
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

/* Register benchmark with expected results */
void init_math_benchmarks(void)
{
  register_benchmark_ex("integer_math", bench_integer_math, 1000, "Integer arithmetic", 512152763);
  /* float_math: (1.0*1.5+2.5)*0.9+0.1 / (()*0.5+0.5) + 1 = ~2.574 -> 2574 */
  register_benchmark_ex("float_math", bench_float_math, 1000, "Floating point math", 2574);
  /* array_sum: sum of i*7+13 for i=0..255 = 231808 */
  register_benchmark_ex("array_sum", bench_array_sum, 100, "Array sum with memory access", 231808);
}
