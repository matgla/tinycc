/*
 * Benchmark library main entry point
 * Extracted from main.c - use benchmark_main() instead of main()
 * This allows linking as a library with different main() implementations
 */

#include "benchmarks.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pico SDK watchdog support */
#ifdef PICO_PLATFORM
#include "hardware/watchdog.h"
#endif

/* Cycle counter interface */
extern void enable_cycle_counter(void);
extern uint64_t get_cycle_count(void);
extern int using_dwt_counter(void);

/* Benchmark function type */
typedef int (*benchmark_func_t)(int iterations);

/* Benchmark registration */
#define MAX_BENCHMARKS 16

typedef struct
{
  const char *name;
  benchmark_func_t func;
  int iterations;
  const char *description;
  int expected_result;
  int verify_status; /* 0=not checked, 1=pass, 2=fail */
} benchmark_t;

static benchmark_t benchmarks[MAX_BENCHMARKS];
static int num_benchmarks = 0;

/* Special marker: no expected result set (skip verification) */
#define NO_EXPECTED_RESULT 0xDEADBEEF

void register_benchmark(const char *name, benchmark_func_t func, int iterations, const char *description)
{
  register_benchmark_ex(name, func, iterations, description, NO_EXPECTED_RESULT);
}

void register_benchmark_ex(const char *name, benchmark_func_t func, int iterations, const char *description,
                           int expected_result)
{
  if (num_benchmarks >= MAX_BENCHMARKS)
    return;
  /* Check for duplicate registration (can happen with constructor attributes) */
  for (int i = 0; i < num_benchmarks; i++)
  {
    if (strcmp(benchmarks[i].name, name) == 0)
      return; /* Already registered */
  }
  benchmarks[num_benchmarks].name = name;
  benchmarks[num_benchmarks].func = func;
  benchmarks[num_benchmarks].iterations = iterations;
  benchmarks[num_benchmarks].description = description;
  benchmarks[num_benchmarks].expected_result = expected_result;
  benchmarks[num_benchmarks].verify_status = VERIFY_NOT_CHECKED;
  num_benchmarks++;
}

int get_benchmark_verify_status(const char *name)
{
  for (int i = 0; i < num_benchmarks; i++)
  {
    if (strcmp(benchmarks[i].name, name) == 0)
    {
      return benchmarks[i].verify_status;
    }
  }
  return VERIFY_NOT_CHECKED;
}

int get_benchmark_expected_result(const char *name)
{
  for (int i = 0; i < num_benchmarks; i++)
  {
    if (strcmp(benchmarks[i].name, name) == 0)
    {
      return benchmarks[i].expected_result;
    }
  }
  return 0;
}

/* Run a single benchmark and return cycle count */
static uint64_t run_benchmark_cycles(const benchmark_t *bench, int iterations)
{
  volatile int result = 0; /* Prevent optimization */

  bench->func(iterations / 10);

  uint64_t start = get_cycle_count();
  result = bench->func(iterations);
  uint64_t end = get_cycle_count();

  /* Use result to prevent optimization */
  (void)result;

  return end - start;
}

/* Guard to prevent double initialization */
static int benchmarks_initialized = 0;

int benchmark_main(void)
{
  /* Disable watchdog to prevent resets during long benchmarks */
#ifdef PICO_PLATFORM
  watchdog_disable();
#endif

  /* Initialize all benchmark modules (only once) */
  if (!benchmarks_initialized)
  {
    benchmarks_initialized = 1;
    init_math_benchmarks();
    init_control_benchmarks();
    init_string_benchmarks();
    init_algorithm_benchmarks();
    init_mibench_benchmarks();
  }

  enable_cycle_counter();

  printf("\n========================================\n");
  printf("ARMv8-M Benchmark Suite\n");
  printf("Compiler: %s (sig=0x%06X)\n", benchmark_compiler_name, benchmark_compiler_sig);
  printf("Build: %s\n", benchmark_compiler_id);
#ifdef __OPTIMIZE__
  printf("Optimization: O1\n");
#else
  printf("Optimization: O0\n");
#endif
  printf("Target: ARM Cortex-M33 (ARMv8-M)\n");
  printf("========================================\n\n");

  if (num_benchmarks == 0)
  {
    printf("No benchmarks registered!\n");
    return 1;
  }

  printf("Running %d benchmarks...\n\n", num_benchmarks);
  fflush(stdout);

  /* Check if cycle counter is working */
  uint64_t test_time = get_cycle_count();
  int have_cycle_counter = (test_time != 0 || using_dwt_counter());

  /* First pass: Verify correctness with known iteration counts */
  printf("Verifying benchmark correctness...\n");
  /* Use volatile to prevent TCC optimization issues with local vars */
  volatile int verify_passed = 0;
  volatile int verify_failed = 0;
  volatile int verify_skipped = 0;

  for (int i = 0; i < num_benchmarks; i++)
  {
    benchmark_t *bench = &benchmarks[i];

    if (bench->expected_result != NO_EXPECTED_RESULT)
    {
      /* Run with registered iteration count to verify result */
      int result = bench->func(bench->iterations);
      if (result == bench->expected_result)
      {
        bench->verify_status = VERIFY_PASS;
        verify_passed++;
      }
      else
      {
        bench->verify_status = VERIFY_FAIL;
        verify_failed++;
        printf("VERIFY FAIL: %s expected %d, got %d\n", bench->name, bench->expected_result, result);
      }
    }
    else
    {
      bench->verify_status = VERIFY_NOT_CHECKED;
      verify_skipped++;
    }
  }

  if (verify_failed > 0)
  {
    printf("\nWARNING: %d benchmark(s) failed verification!\n", verify_failed);
  }
  if (verify_passed > 0)
  {
    printf("%d benchmark(s) passed verification, ", verify_passed);
    if (verify_skipped > 0)
    {
      printf("%d skipped (no expected value)\n\n", verify_skipped);
    }
    else
    {
      printf("\n\n");
    }
  }

  /* Second pass: Run performance measurements */
  if (have_cycle_counter)
  {
    printf("%-20s %12s %12s %12s %8s\n", "Benchmark", "Iterations", "Cycles/iter", "Result", "Verify");
    printf("%-20s %12s %12s %12s %8s\n", "---------", "----------", "-----------", "------", "------");
    fflush(stdout);
  }
  else
  {
    printf("Note: DWT cycle counter not available (running in QEMU/simulator)\n");
    printf("%-20s %12s %12s %8s\n", "Benchmark", "Iterations", "Result", "Verify");
    printf("%-20s %12s %12s %8s\n", "---------", "----------", "------", "------");
  }

  for (int i = 0; i < num_benchmarks; i++)
  {
    const benchmark_t *bench = &benchmarks[i];
    int iterations = bench->iterations;

    /* Avoid complex ternary chain - TCC may have codegen issues with it */
    const char *verify_str;
    if (bench->verify_status == VERIFY_PASS)
    {
      verify_str = "PASS";
    }
    else if (bench->verify_status == VERIFY_FAIL)
    {
      verify_str = "FAIL";
    }
    else if (bench->verify_status == VERIFY_NOT_CHECKED)
    {
      verify_str = "SKIP";
    }
    else
    {
      verify_str = "?";
    }

    if (have_cycle_counter)
    {
      /* Run with registered iteration count */
      uint64_t cycles = run_benchmark_cycles(bench, iterations);
      int result = bench->func(1);
      /* Small delay after TCC function returns */
      for (volatile int delay = 0; delay < 100000; delay++)
      {
      }
      /* Split the printf into multiple simple ones */
      printf("%-20s ", bench->name);
      fflush(stdout);
      printf("%12d ", iterations);
      fflush(stdout);
      printf("%12d ", (int)(cycles & 0xFFFFFFFF)); /* Just print raw cycles */
      fflush(stdout);
      printf("%12d ", result);
      fflush(stdout);
      printf("%8s\n", verify_str);
      fflush(stdout);
    }
    else
    {
      /* Just run registered iterations and show result */
      int result = bench->func(iterations);
      printf("%-20s %12d %12d %8s\n", bench->name, iterations, result, verify_str);
      fflush(stdout);
    }
  }

  printf("\n========================================\n");
  printf("Benchmark complete\n");
  printf("========================================\n");
  fflush(stdout);

  return 0;
}
