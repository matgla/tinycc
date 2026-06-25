/*
 * MiBench Qsort Adapter for RP2350 Benchmark Suite
 *
 * Uses the original string-compare workload pattern with embedded data.
 */

#include "../benchmarks.h"
#include <stdlib.h>
#include <string.h>

#define MIBENCH_QSORT_WORD_COUNT 32

struct myStringStruct
{
  char qstring[128];
};

static const char *const qsort_words[MIBENCH_QSORT_WORD_COUNT] = {
    "photonic",
    "crystals",
    "microwave",
    "antennas",
    "conductive",
    "surface",
    "texture",
    "impedance",
    "current",
    "reflection",
    "compiler",
    "tinycc",
    "armv8m",
    "cortex",
    "embedded",
    "benchmark",
    "deterministic",
    "verification",
    "adjacency",
    "checksum",
    "algorithm",
    "security",
    "network",
    "telecomm",
    "office",
    "automotive",
    "iteration",
    "sorting",
    "pointer",
    "register",
    "throughput",
    "latency",
};

static int compare_qsort_strings(const void *elem1, const void *elem2)
{
  const struct myStringStruct *left = (const struct myStringStruct *)elem1;
  const struct myStringStruct *right = (const struct myStringStruct *)elem2;
  int result = strcmp(left->qstring, right->qstring);

  if (result < 0)
  {
    return 1;
  }
  if (result > 0)
  {
    return -1;
  }

  return 0;
}

int bench_mibench_qsort(int iterations)
{
  struct myStringStruct array[MIBENCH_QSORT_WORD_COUNT];
  int checksum = 0;

  for (int iteration = 0; iteration < iterations; iteration++)
  {
    for (int index = 0; index < MIBENCH_QSORT_WORD_COUNT; index++)
    {
      strcpy(array[index].qstring, qsort_words[(index + iteration) % MIBENCH_QSORT_WORD_COUNT]);
    }

    qsort(array, MIBENCH_QSORT_WORD_COUNT, sizeof(array[0]), compare_qsort_strings);

    checksum = 0;
    for (int index = 0; index < MIBENCH_QSORT_WORD_COUNT; index++)
    {
      checksum += (unsigned char)array[index].qstring[0] * (index + 1);
      checksum += (int)strlen(array[index].qstring);
    }
  }

  return checksum;
}

void init_mibench_qsort(void)
{
  register_benchmark_ex("mibench_qsort", bench_mibench_qsort, 200, "MiBench: qsort string workload", 54258);
}
