/*
 * MiBench SHA Adapter for RP2350 Benchmark Suite
 *
 * Adapts the MiBench SHA benchmark to work with our benchmark harness.
 * Uses synthetic input data suitable for embedded targets.
 */

#include "benchmarks.h"
#include <string.h>

/* Include SHA implementation directly - provides SHA_INFO and functions */
#include "../mibench/security/sha/sha.c"

/* Synthetic test data (deterministic, reproducible) */
static const char sha_test_data[] = "The quick brown fox jumps over the lazy dog. "
                                    "Pack my box with five dozen liquor jugs. "
                                    "How vexingly quick daft zebras jump! "
                                    "The five boxing wizards jump quickly. "
                                    "Sphinx of black quartz, judge my vow.";

/* Run SHA on synthetic data */
int bench_mibench_sha(int iterations)
{
  SHA_INFO sha_info;
  int i, j;
  volatile int checksum = 0;

  /* Create a larger input by repeating test data */
  char input_buffer[1024];
  int data_len = 0;

  /* Fill buffer with repeated test data */
  while (data_len + (int)sizeof(sha_test_data) < (int)sizeof(input_buffer))
  {
    memcpy(input_buffer + data_len, sha_test_data, sizeof(sha_test_data) - 1);
    data_len += sizeof(sha_test_data) - 1;
  }

  /* Run multiple iterations */
  for (i = 0; i < iterations; i++)
  {
    /* Modify input slightly per iteration to prevent optimization */
    input_buffer[0] = (char)('A' + (i % 26));

    /* Compute SHA hash */
    sha_init(&sha_info);
    sha_update(&sha_info, (BYTE *)input_buffer, data_len);
    sha_final(&sha_info);

    /* Accumulate checksum from digest */
    checksum = 0;
    for (j = 0; j < 5; j++)
    {
      checksum += (int)(sha_info.digest[j] & 0xFFFF);
    }
  }

  return checksum;
}

void init_mibench_sha(void)
{
  /* mibench_sha: checksum from last iteration's SHA digest - skip verify for now */
  register_benchmark("mibench_sha", bench_mibench_sha, 50, "MiBench: SHA-1 hash");
}
