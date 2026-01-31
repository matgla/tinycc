/*
 * TCC Bug: Float math loop produces incorrect result
 * 
 * This test reproduces the benchmark float_math failure.
 * TCC returns 4999 (or 8999) instead of expected 2574.
 * 
 * Expected: 2574 (GCC produces this correctly)
 * Actual with TCC: 4999 or 8999 (depending on optimization)
 */

#include <stdio.h>

/* The benchmark function from bench_math.c */
int bench_float_math(int iterations) {
    volatile float result = 1.0f;
    float a = 1.5f;
    float b = 2.5f;
    
    for (int i = 0; i < iterations; i++) {
        result = result * a + b;
        result = result * 0.9f + 0.1f;
        /* Avoid sqrtf for now - TCC float support issue */
        result = result / (result * 0.5f + 0.5f) + 1.0f;
        a = result * 0.5f;
        b = result * 0.3f;
    }
    
    return (int)(result * 1000);
}

int main(void) {
    /* Run with 1 iteration to get deterministic result */
    int result = bench_float_math(1);
    
    printf("float_math(1) = %d\n", result);
    printf("Expected: 2574\n");
    
    /* Return 0 on success (matching expected), 1 on failure */
    if (result == 2574) {
        printf("PASS: Result matches expected\n");
        return 0;
    } else {
        printf("FAIL: Expected 2574, got %d\n", result);
        return 1;
    }
}
