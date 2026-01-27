/*
 * Algorithm benchmark
 * Tests: recursion, sorting, pointer manipulation
 */

#include <stddef.h>
#include "benchmarks.h"

/* Fibonacci - tests recursion depth and call overhead */
static int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

int bench_fibonacci(int iterations) {
    volatile int result = 0;
    
    for (int i = 0; i < iterations; i++) {
        result = fib(20);  /* fib(20) = 6765 */
    }
    
    return result;
}

/* Bubble sort - tests nested loops and array access */
int bench_bubble_sort(int iterations) {
    int arr[64];
    volatile int checksum = 0;
    
    for (int iter = 0; iter < iterations; iter++) {
        /* Initialize with pseudo-random values */
        for (int i = 0; i < 64; i++) {
            arr[i] = (63 - i) * 7 + (iter * 13);
        }
        
        /* Bubble sort */
        for (int i = 0; i < 63; i++) {
            for (int j = 0; j < 63 - i; j++) {
                if (arr[j] > arr[j + 1]) {
                    int temp = arr[j];
                    arr[j] = arr[j + 1];
                    arr[j + 1] = temp;
                }
            }
        }
        
        /* Checksum of sorted array */
        checksum = 0;
        for (int i = 0; i < 64; i++) {
            checksum += arr[i] * i;
        }
    }
    
    return checksum;
}

/* Linked list traversal - tests pointer chasing */
struct node {
    int value;
    struct node *next;
};

static struct node nodes[100];

int bench_linked_list(int iterations) {
    volatile int sum = 0;
    
    /* Initialize linked list */
    for (int i = 0; i < 100; i++) {
        nodes[i].value = i * 3 + 7;
        nodes[i].next = (i < 99) ? &nodes[i + 1] : NULL;
    }
    
    for (int iter = 0; iter < iterations; iter++) {
        sum = 0;
        struct node *p = &nodes[0];
        while (p) {
            sum += p->value;
            p = p->next;
        }
        
        /* Modify values for next iteration */
        p = &nodes[0];
        while (p) {
            p->value = (p->value * 31 + 17) & 0xFFFF;
            p = p->next;
        }
    }
    
    return sum;
}

/* Register benchmark */
void init_algorithm_benchmarks(void) {
    register_benchmark("fibonacci", bench_fibonacci, 100, "Recursive fibonacci(20)");
    register_benchmark("bubble_sort", bench_bubble_sort, 200, "Bubble sort 64 elements");
    register_benchmark("linked_list", bench_linked_list, 1000, "Linked list traversal");
}
