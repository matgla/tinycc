/*
 * Control flow benchmark
 * Tests: branches, loops, conditionals, function calls
 */

#include "benchmarks.h"

/* Helper functions for call benchmark */
static int NOINLINE func_a(int x) {
    return x * 3 + 7;
}

static int NOINLINE func_b(int x) {
    return x * 5 - 3;
}

static int NOINLINE func_c(int x) {
    return (x << 2) + 1;
}

/* Function call benchmark */
int bench_function_calls(int iterations) {
    volatile int result = 0;
    
    for (int i = 0; i < iterations; i++) {
        result = func_a(i);
        result = func_b(result);
        result = func_c(result);
        result = func_a(result);
        result = func_b(result);
    }
    
    return result;
}

/* Conditional benchmark */
int bench_conditionals(int iterations) {
    volatile int result = 0;
    
    for (int i = 0; i < iterations; i++) {
        if (i & 1) {
            result += i * 3;
        } else if (i % 3 == 0) {
            result -= i;
        } else {
            result ^= i;
        }
        
        if (result > 1000000) {
            result = result >> 3;
        } else if (result < -1000000) {
            result = -result;
        }
    }
    
    return result;
}

/* Switch statement benchmark */
int bench_switch(int iterations) {
    volatile int result = 0;
    
    for (int i = 0; i < iterations; i++) {
        switch (i & 7) {
            case 0: result += i; break;
            case 1: result -= i; break;
            case 2: result *= 3; break;
            case 3: result /= 2; break;
            case 4: result ^= i; break;
            case 5: result &= i; break;
            case 6: result |= i; break;
            case 7: result = ~result; break;
        }
    }
    
    return result;
}

/* Register benchmark */
void init_control_benchmarks(void) {
    register_benchmark("function_calls", bench_function_calls, 5000, "Function call overhead");
    register_benchmark("conditionals", bench_conditionals, 10000, "If-else branches");
    register_benchmark("switch_stmt", bench_switch, 10000, "Switch statement");
}
