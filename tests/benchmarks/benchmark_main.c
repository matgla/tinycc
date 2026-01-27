/*
 * Benchmark library main entry point
 * Extracted from main.c - use benchmark_main() instead of main()
 * This allows linking as a library with different main() implementations
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "benchmarks.h"

/* Semihosting interface for timing - may be platform-specific */
extern void enable_cycle_counter(void);
extern unsigned int get_cycle_count_low(void);

/* Benchmark function type */
typedef int (*benchmark_func_t)(int iterations);

/* Benchmark registration */
#define MAX_BENCHMARKS 16

typedef struct {
    const char *name;
    benchmark_func_t func;
    int default_iterations;
    const char *description;
} benchmark_t;

static benchmark_t benchmarks[MAX_BENCHMARKS];
static int num_benchmarks = 0;

void register_benchmark(const char *name, benchmark_func_t func,
                        int default_iterations, const char *description) {
    if (num_benchmarks >= MAX_BENCHMARKS) return;
    benchmarks[num_benchmarks].name = name;
    benchmarks[num_benchmarks].func = func;
    benchmarks[num_benchmarks].default_iterations = default_iterations;
    benchmarks[num_benchmarks].description = description;
    num_benchmarks++;
}

/* Run a single benchmark and return cycle count */
static unsigned int run_benchmark_cycles(const benchmark_t *bench, int iterations) {
    volatile int result = 0;  /* Prevent optimization */

    /* Warmup */
    bench->func(iterations / 10);

    /* Actual measurement */
    unsigned int start = get_cycle_count_low();
    result = bench->func(iterations);
    unsigned int end = get_cycle_count_low();

    /* Use result to prevent optimization */
    (void)result;

    return end - start;
}

/* Calibrate iterations to run for approximately target_cycles */
static int calibrate_iterations(const benchmark_t *bench, unsigned int target_cycles) {
    int iterations = bench->default_iterations;
    unsigned int cycles;

    /* Try up to 3 times to get a stable measurement */
    for (int attempt = 0; attempt < 3; attempt++) {
        cycles = run_benchmark_cycles(bench, iterations);

        if (cycles == 0) {
            /* Cycle counter not available, use default iterations */
            return bench->default_iterations;
        }

        if (cycles >= target_cycles / 2 && cycles <= target_cycles * 2) {
            /* Good enough */
            return iterations;
        }

        if (cycles < target_cycles / 10) {
            /* Too fast, increase iterations */
            iterations *= 10;
        } else if (cycles < target_cycles) {
            /* Slightly too fast */
            iterations = (int)((long long)iterations * target_cycles / cycles);
        } else {
            /* Too slow, decrease iterations */
            iterations = (int)((long long)iterations * target_cycles / cycles);
            if (iterations < 10) iterations = 10;
        }
    }

    return iterations;
}

/* Verify which compiler was actually used */
static int get_compiler_signature(void) {
#ifdef __TINYC__
    /* TCC-specific: return a different value */
    return 0x544343;  /* "TCC" in hex */
#else
    /* GCC/Clang */
    return 0x474343;  /* "GCC" in hex */
#endif
}

int benchmark_main(void) {
    /* Early debug print - before anything else */
    printf("\r\n[DEBUG] benchmark_main() started\r\n");

    /* Initialize all benchmark modules */
    printf("[DEBUG] init_math_benchmarks...\r\n");
    init_math_benchmarks();
    printf("[DEBUG] init_control_benchmarks...\r\n");
    init_control_benchmarks();
    printf("[DEBUG] init_string_benchmarks...\r\n");
    init_string_benchmarks();
    printf("[DEBUG] init_algorithm_benchmarks...\r\n");
    init_algorithm_benchmarks();

    printf("[DEBUG] enable_cycle_counter...\r\n");
    enable_cycle_counter();

    printf("[DEBUG] getting compiler signature...\r\n");
    /* Verify compiler signature matches macro */
    int sig = get_compiler_signature();
    printf("[DEBUG] sig=0x%06X\r\n", sig);

    const char* compiler_name;
#ifdef __TINYC__
    compiler_name = "TCC";
    if (sig != 0x544343) {
        printf("ERROR: Compiler mismatch! Expected TCC but got different code\n");
        return 1;
    }
#else
    compiler_name = "GCC";
    if (sig != 0x474343) {
        printf("ERROR: Compiler mismatch! Expected GCC but got different code\n");
        return 1;
    }
#endif

    printf("[DEBUG] printing banner...\r\n");
    printf("\n========================================\n");
    printf("ARMv8-M Benchmark Suite\n");
    printf("Compiler: %s (sig=0x%06X)\n", compiler_name, sig);
    printf("Build: %s\n",
#ifdef __TINYC__
        "TINYCC"
#else
        "GCC"
#endif
    );
#ifdef __OPTIMIZE__
    printf("Optimization: O1\n");
#else
    printf("Optimization: O0\n");
#endif
    printf("Target: ARM Cortex-M33 (ARMv8-M)\n");
    printf("========================================\n\n");

    if (num_benchmarks == 0) {
        printf("No benchmarks registered!\n");
        return 1;
    }

    printf("Running %d benchmarks...\n\n", num_benchmarks);

    /* Check if cycle counter is working */
    unsigned int test_cycles = get_cycle_count_low();
    int have_cycle_counter = (test_cycles != 0);

    if (have_cycle_counter) {
        printf("%-20s %12s %12s %12s\n", "Benchmark", "Iterations", "Cycles/iter", "Result");
        printf("%-20s %12s %12s %12s\n", "---------", "----------", "-----------", "------");
    } else {
        printf("Note: DWT cycle counter not available (running in QEMU/simulator)\n");
        printf("%-20s %12s %12s\n", "Benchmark", "Iterations", "Result");
        printf("%-20s %12s %12s\n", "---------", "----------", "------");
    }

    for (int i = 0; i < num_benchmarks; i++) {
        const benchmark_t *bench = &benchmarks[i];

        if (have_cycle_counter) {
            /* Calibrate to run for approximately 100,000 cycles */
            int iterations = calibrate_iterations(bench, 100000);
            unsigned int cycles = run_benchmark_cycles(bench, iterations);
            double cycles_per_iter = cycles / (double)iterations;

            /* Run once more to get a result value */
            int result = bench->func(1);

            printf("%-20s %12d %12.2f %12d\n", bench->name, iterations, cycles_per_iter, result);
        } else {
            /* Just run default iterations and show result */
            int result = bench->func(bench->default_iterations);
            printf("%-20s %12d %12d\n", bench->name, bench->default_iterations, result);
        }
    }

    printf("\n========================================\n");
    printf("Benchmark complete\n");
    printf("========================================\n");

    return 0;
}
