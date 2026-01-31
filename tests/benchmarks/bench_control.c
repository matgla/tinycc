/*
 * Control flow benchmark
 * Tests: branches, loops, conditionals, function calls
 * All benchmarks return deterministic results independent of iteration count
 */

#include "benchmarks.h"
#include <stdio.h>

/* Helper functions for call benchmark */
static int NOINLINE func_a(int x)
{
  return x * 3 + 7;
}

static int NOINLINE func_b(int x)
{
  return x * 5 - 3;
}

static int NOINLINE func_c(int x)
{
  return (x << 2) + 1;
}

/* Function call benchmark - deterministic result */
int bench_function_calls(int iterations)
{
  int result = 0;

  for (int n = 0; n < iterations; n++)
  {
    result = func_a(100);
    result = func_b(result);
    result = func_c(result);
    result = func_a(result);
    result = func_b(result);
  }

  return result;
}

/* Conditional benchmark - deterministic result */
int bench_conditionals(int iterations)
{
  int r = 0;

  for (int n = 0; n < iterations; n++)
  {
    int i = 42; /* Fixed value for deterministic result */

    r = 1234; /* Reset each iteration */
    if (i & 1)
    {
      r += i * 3;
    }
    else if (i % 3 == 0)
    {
      r -= i;
    }
    else
    {
      r ^= i;
    }

    if (r > 1000000)
    {
      r = r >> 3;
    }
    else if (r < -1000000)
    {
      r = -r;
    }
  }

  return r;
}

/* Switch statement benchmark - deterministic result */
int bench_switch(int iterations)
{
  int r = 0;

  /* No debug prints - check if TCC code without printf works */

  for (int n = 0; n < iterations; n++)
  {
    int i = 7; /* Fixed value for deterministic result */

    r = 1000; /* Reset each iteration */

    switch (i)
    {
    case 0:
      r += i + 1;
      break;
    case 1:
      r -= i;
      break;
    case 2:
      r *= 2;
      r /= 2;
      r += 1;
      break;
    case 3:
      r = r / 2 + 1;
      break;
    case 4:
      r ^= i;
      break;
    case 5:
      r &= (0xFFFF + i);
      break;
    case 6:
      r |= (i & 0x0F);
      break;
    case 7:
      r = (r ^ 0xFF) ^ 0xFF;
      break;
    }
  }

  /* No printf at end either */
  return r;
}

/* Register benchmark with expected results */
void init_control_benchmarks(void)
{
  register_benchmark_ex("function_calls", bench_function_calls, 1000, "Function call overhead", 91967);
  /* conditionals: 1234 + 42*3 = 1360 (i=42 is odd, so r += i*3) */
  register_benchmark_ex("conditionals", bench_conditionals, 1000, "If-else branches", 1192);
  /* switch_stmt: case 7: (1000 ^ 0xFF) ^ 0xFF = 1000 */
  register_benchmark_ex("switch_stmt", bench_switch, 1000, "Switch statement", 1000);
}
