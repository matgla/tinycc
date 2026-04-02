/* MiBench SHA - regression detection for -O2
 * SHA-1 hash on synthetic input data.
 */
#include <stdio.h>
#include <string.h>

/* Include SHA implementation from mibench submodule */
#include "../benchmarks/mibench/security/sha/sha.c"

static unsigned char sha_input_buffer[256];

int bench_mibench_sha(void)
{
    SHA_INFO sha_info;
    volatile int checksum = 0;
    int i, j;
    int iterations = 50;

    /* Initialize static buffer with deterministic data */
    for (i = 0; i < 256; i++) {
        sha_input_buffer[i] = (unsigned char)((i * 7 + 13) & 0xFF);
    }

    for (i = 0; i < iterations; i++) {
        sha_input_buffer[0] = (unsigned char)('A' + (i % 26));

        sha_init(&sha_info);
        sha_update(&sha_info, sha_input_buffer, 256);
        sha_final(&sha_info);

        checksum = 0;
        for (j = 0; j < 5; j++) {
            checksum += (int)(sha_info.digest[j] & 0xFF);
        }
    }

    return checksum;
}

int main(void)
{
    printf("sha: %d\n", bench_mibench_sha());
    return 0;
}
