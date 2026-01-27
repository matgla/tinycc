/*
 * Mathematical computation benchmark
 * Tests: floating point, integer math, loops
 */

#include "benchmarks.h"

/* Integer math benchmark */
int bench_integer_math(int iterations) {
    volatile int result = 0;
    int a = 12345;
    int b = 6789;
    
    for (int i = 0; i < iterations; i++) {
        result = a * b + (a >> 3) - (b << 2);
        result += (result * 31) >> 5;
        result ^= (result << 13);
        result += i;
        a = result + 1;
        b = result ^ a;
    }
    
    return result;
}

/* Floating point math benchmark */
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

/* Array sum benchmark - tests memory access patterns */
int bench_array_sum(int iterations) {
    static int arr[256];
    volatile int sum = 0;
    
    /* Initialize */
    for (int i = 0; i < 256; i++) {
        arr[i] = i * 7 + 13;
    }
    
    for (int iter = 0; iter < iterations; iter++) {
        sum = 0;
        for (int i = 0; i < 256; i++) {
            sum += arr[i];
        }
        /* Modify array for next iteration */
        for (int i = 0; i < 256; i++) {
            arr[i] = (arr[i] * 31 + 17) & 0xFF;
        }
    }
    
    return sum;
}

/* Register benchmark */
void init_math_benchmarks(void) {
    register_benchmark("integer_math", bench_integer_math, 10000, "Integer arithmetic");
    register_benchmark("float_math", bench_float_math, 5000, "Floating point math");
    register_benchmark("array_sum", bench_array_sum, 1000, "Array sum with memory access");
}
