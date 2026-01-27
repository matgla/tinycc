/*
 * Benchmark registration header
 */

#ifndef BENCHMARKS_H
#define BENCHMARKS_H

#ifdef __TINYC__
#define NOINLINE
#define CONSTRUCTOR
#else
#define NOINLINE __attribute__((noinline))
#define CONSTRUCTOR __attribute__((constructor))
#endif

/* Benchmark function type */
typedef int (*benchmark_func_t)(int iterations);

/* Benchmark registration function */
void register_benchmark(const char *name, benchmark_func_t func, 
                        int default_iterations, const char *description);

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

#endif /* BENCHMARKS_H */
