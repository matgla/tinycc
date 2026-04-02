/*
 * MiBench Bitcount Adapter for RP2350 Benchmark Suite
 *
 * Tests various bit counting algorithms.
 */

#include "../benchmarks.h"

/* Bit counting functions from MiBench */

/* Optimized 1 bit/loop counter */
static int bit_count(long x)
{
  int n = 0;
  while (x)
  {
    n++;
    x &= x - 1;
  }
  return n;
}

/* Ratko's mystery algorithm */
static int bitcount(long i)
{
  i = ((i & 0xAAAAAAAAL) >> 1) + (i & 0x55555555L);
  i = ((i & 0xCCCCCCCCL) >> 2) + (i & 0x33333333L);
  i = ((i & 0xF0F0F0F0L) >> 4) + (i & 0x0F0F0F0FL);
  i = ((i & 0xFF00FF00L) >> 8) + (i & 0x00FF00FFL);
  i = ((i & 0xFFFF0000L) >> 16) + (i & 0x0000FFFFL);
  return (int)i;
}

/* Shift and count bits */
static int bit_shifter(long int x)
{
  int i, n;
  for (i = n = 0; x && (i < 32); ++i, x >>= 1)
    n += (int)(x & 1L);
  return n;
}

/* Run all bit counting algorithms */
int bench_mibench_bitcount(int iterations)
{
  volatile long n = 0;
  long j, seed;

  for (j = 0, seed = 0x12345678; j < iterations; j++, seed += 13)
  {
    /* Run all three algorithms and accumulate results */
    n += bit_count(seed);
    n += bitcount(seed);
    n += bit_shifter(seed);
  }

  return (int)n;
}

void init_mibench_bitcount(void)
{
  /* mibench_bitcount: sum of bit counts - skip verify for now */
  register_benchmark("mibench_bitcount", bench_mibench_bitcount, 1000, "MiBench: Bit counting algorithms");
}
