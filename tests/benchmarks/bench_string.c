/*
 * String manipulation benchmark
 * Tests: memcpy, strcpy, strlen, string comparisons
 */

#include <string.h>
#include "benchmarks.h"

/* String copy benchmark */
int bench_strcpy(int iterations) {
    char src[256] = "The quick brown fox jumps over the lazy dog. "
                    "Pack my box with five dozen liquor jugs. "
                    "How vexingly quick daft zebras jump!";
    char dst[256];
    volatile int total_len = 0;
    
    for (int i = 0; i < iterations; i++) {
        strcpy(dst, src);
        total_len += strlen(dst);
        /* Modify src slightly */
        src[0] = 'A' + (i % 26);
    }
    
    return total_len;
}

/* Memory copy benchmark */
int bench_memcpy(int iterations) {
    char src[512];
    char dst[512];
    volatile int checksum = 0;
    
    /* Initialize source */
    for (int i = 0; i < 512; i++) {
        src[i] = (char)(i * 7 + 13);
    }
    
    for (int i = 0; i < iterations; i++) {
        memcpy(dst, src, 256);
        memcpy(dst + 256, src, 128);
        
        /* Simple checksum */
        checksum = 0;
        for (int j = 0; j < 256; j++) {
            checksum += dst[j];
        }
        
        /* Modify source */
        src[0] = (char)i;
    }
    
    return checksum;
}

/* String comparison benchmark */
int bench_strcmp(int iterations) {
    const char *strings[] = {
        "alpha", "beta", "gamma", "delta", "epsilon",
        "zeta", "eta", "theta", "iota", "kappa"
    };
    int num_strings = sizeof(strings) / sizeof(strings[0]);
    volatile int result = 0;
    
    for (int i = 0; i < iterations; i++) {
        for (int j = 0; j < num_strings; j++) {
            for (int k = 0; k < num_strings; k++) {
                result += strcmp(strings[j], strings[k]);
            }
        }
    }
    
    return result;
}

/* Register benchmark */
void init_string_benchmarks(void) {
    register_benchmark("strcpy", bench_strcpy, 5000, "String copy operations");
    register_benchmark("memcpy", bench_memcpy, 2000, "Memory copy operations");
    register_benchmark("strcmp", bench_strcmp, 500, "String comparisons");
}
