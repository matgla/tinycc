/*
 * MiBench SHA Adapter - Small stack version for TCC compatibility
 * 
 * Reduced stack usage to avoid TCC stack alignment issues.
 */

#include "benchmarks.h"
#include <string.h>

/* Include SHA implementation */
#include "../mibench/security/sha/sha.c"

/* Smaller test buffer to reduce stack usage */
static unsigned char sha_input_buffer[256];  /* Reduced from 1KB to 256B */
static const char sha_test_data[] = 
    "The quick brown fox jumps over the lazy dog. "
    "Pack my box with five dozen liquor jugs.";

int bench_mibench_sha(int iterations)
{
    SHA_INFO sha_info;
    volatile int checksum = 0;
    int i, j;
    
    /* Initialize static buffer */
    for (i = 0; i < 256; i++) {
        sha_input_buffer[i] = (unsigned char)((i * 7 + 13) & 0xFF);
    }
    
    for (i = 0; i < iterations; i++) {
        /* Modify input */
        sha_input_buffer[0] = (unsigned char)('A' + (i % 26));
        
        /* Compute SHA */
        sha_init(&sha_info);
        sha_update(&sha_info, sha_input_buffer, 256);
        sha_final(&sha_info);
        
        /* Accumulate checksum */
        checksum = 0;
        for (j = 0; j < 5; j++) {
            checksum += (int)(sha_info.digest[j] & 0xFF);
        }
    }
    
    return checksum;
}

void init_mibench_sha(void)
{
    register_benchmark_ex("mibench_sha", bench_mibench_sha, 50,
                          "MiBench: SHA-1 hash (small)", 0xDEADBEEF);
}
