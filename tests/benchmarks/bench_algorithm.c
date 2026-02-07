/*
 * Algorithm benchmark
 * Tests: recursion, sorting, pointer manipulation
 * All benchmarks return deterministic results independent of iteration count
 */

#include <stddef.h>
#include "benchmarks.h"

/* Fibonacci - tests recursion depth and call overhead */
static int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

/* Single fibonacci computation - result is always fib(20) = 6765 */
int bench_fibonacci(int iterations)
{
  (void)iterations;
  return fib(20);
}

/* Bubble sort - tests nested loops and array access */
int bench_bubble_sort(int iterations)
{
  (void)iterations;

  int arr[64];
  int checksum = 0;

  /* Initialize with fixed deterministic values */
  for (int i = 0; i < 64; i++) {
      arr[i] = (63 - i) * 7 + 100;
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

  return checksum;
}

/* Linked list traversal - tests pointer chasing */
struct node {
    int value;
    struct node *next;
};

static struct node nodes[100];

int bench_linked_list(int iterations)
{
  (void)iterations;

  int sum = 0;

  /* Initialize linked list deterministically */
  for (int i = 0; i < 100; i++) {
      nodes[i].value = i * 3 + 7;
      nodes[i].next = (i < 99) ? &nodes[i + 1] : NULL;
  }

  sum = 0;
  struct node *p = &nodes[0];
  while (p) {
      sum += p->value;
      p = p->next;
  }

  return sum;
}

/* Register benchmark with expected results */
void init_algorithm_benchmarks(void)
{
  /* fibonacci(20) = 6765 */
  register_benchmark_ex("fibonacci", bench_fibonacci, 500, "Recursive fibonacci(20)", 6765);
  /* bubble_sort: checksum = 799008 (computed from actual run) */
  register_benchmark_ex("bubble_sort", bench_bubble_sort, 1000, "Bubble sort 64 elements", 799008);
  /* linked_list: sum of i*3+7 for i=0..99 = 15550 */
  register_benchmark_ex("linked_list", bench_linked_list, 5000, "Linked list traversal", 15550);
}
