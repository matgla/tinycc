/*
 * Benchmark registration header
 */

#ifndef BENCHMARKS_H
#define BENCHMARKS_H

#ifdef __TINYC__
#define NOINLINE
#else
#define NOINLINE __attribute__((noinline))
#endif

/* Benchmark function type */
typedef int (*benchmark_func_t)(int iterations);

/* Verification function type - returns 1 if result is valid, 0 otherwise */
typedef int (*verify_func_t)(int result);

/* Benchmark registration function with expected result */
void register_benchmark(const char *name, benchmark_func_t func, int iterations, const char *description);

/* Registration with expected result for verification */
void register_benchmark_ex(const char *name, benchmark_func_t func, int iterations, const char *description,
                           int expected_result);

/* Special marker for benchmarks without expected result verification */
#define NO_EXPECTED_RESULT 0xDEADBEEF

/* Verification result codes */
#define VERIFY_NOT_CHECKED -1
#define VERIFY_PASS 1
#define VERIFY_FAIL 2

/* Get verification status for a benchmark */
int get_benchmark_verify_status(const char *name);

/* Get expected result for a benchmark */
int get_benchmark_expected_result(const char *name);

/* Compiler identification - defined in benchmark library (bench_math.c) */
extern const char *benchmark_compiler_name;
extern const int benchmark_compiler_sig;
extern const char *benchmark_compiler_id;

/* External declarations for all benchmarks */
int bench_integer_math(int iterations);
int bench_float_math(int iterations);
int bench_array_sum(int iterations);
int bench_function_calls(int iterations);
int bench_conditionals(int iterations);
int bench_switch(int iterations);
int bench_strcpy(int iterations);
int bench_memcpy(int iterations);
int bench_strcmp(int iterations);
int bench_fibonacci(int iterations);
int bench_bubble_sort(int iterations);
int bench_linked_list(int iterations);

/* Registration functions */
void init_math_benchmarks(void);
void init_control_benchmarks(void);
void init_string_benchmarks(void);
void init_algorithm_benchmarks(void);
void init_mibench_benchmarks(void);

#endif /* BENCHMARKS_H */
