/*
 * String manipulation benchmark
 * Tests: memcpy, strcpy, strlen, string comparisons
 * All benchmarks return deterministic results independent of iteration count
 */

#include "benchmarks.h"
#include <string.h>

/* String copy benchmark - deterministic */
int bench_strcpy(int iterations)
{
  /* Fixed string - no modification */
  char src[256] = "The quick brown fox jumps over the lazy dog. "
                  "Pack my box with five dozen liquor jugs. "
                  "How vexingly quick daft zebras jump!";
  char dst[256];
  int len = 0;

  for (int n = 0; n < iterations; n++)
  {
    strcpy(dst, src);
    len = strlen(dst);
  }

  return len;
}

/* Memory copy benchmark - deterministic */
int bench_memcpy(int iterations)
{
  char src[512];
  char dst[512];
  int checksum = 0;

  for (int i = 0; i < 512; i++)
  {
    src[i] = (char)((i * 7 + 13) & 0xFF);
  }

  for (int n = 0; n < iterations; n++)
  {
    memcpy(dst, src, 256);
    memcpy(dst + 256, src, 128);

    checksum = 0;
    for (int j = 0; j < 256; j++)
    {
      checksum += (unsigned char)dst[j];
    }
  }

  return checksum;
}

/* String comparison benchmark - deterministic */
int bench_strcmp(int iterations)
{
  const char *s1 = "alpha";
  const char *s2 = "beta";
  int result = 0;

  for (int n = 0; n < iterations; n++)
  {
    result = strcmp(s1, s2);
  }

  return result + 100;
}

/* String length scan benchmark - deterministic */
int bench_strlen_scan(int iterations)
{
  static const char *words[] = {
      "benchmark",
      "tinycc",
      "cortex-m33",
      "rp2350",
      "deterministic",
      "verification",
  };
  int total = 0;

  for (int n = 0; n < iterations; n++)
  {
    total = 0;
    for (int i = 0; i < 6; i++)
    {
      total += (int)strlen(words[i]) * (i + 3);
    }
  }

  return total;
}

/* Register benchmark with expected results */
void init_string_benchmarks(void)
{
  register_benchmark_ex("strcpy", bench_strcpy, 1000, "String copy operations", 122);
  register_benchmark_ex("memcpy", bench_memcpy, 1000, "Memory copy operations", 32640);
  register_benchmark_ex("strcmp", bench_strcmp, 1000, "String comparisons", 99);
  register_benchmark_ex("strlen_scan", bench_strlen_scan, 2000, "Repeated strlen scans", 324);
}
