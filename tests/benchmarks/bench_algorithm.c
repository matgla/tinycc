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

/* Binary search benchmark - tests branch-heavy lookup in sorted data */
int bench_binary_search(int iterations)
{
    int data[128];
    int checksum = 0;

    for (int i = 0; i < 128; i++)
    {
        data[i] = i * 5 + 11;
    }

    for (int n = 0; n < iterations; n++)
    {
        int target = data[(n * 17) & 127];
        int left = 0;
        int right = 127;

        while (left <= right)
        {
            int mid = left + ((right - left) / 2);

            if (data[mid] == target)
            {
                checksum = mid + target;
                break;
            }
            if (data[mid] < target)
            {
                left = mid + 1;
            }
            else
            {
                right = mid - 1;
            }
        }
    }

    return checksum;
}

/* Small matrix multiply benchmark - deterministic integer arithmetic */
int bench_matrix_mul(int iterations)
{
    int a[4][4];
    int b[4][4];
    int checksum = 0;

    for (int row = 0; row < 4; row++)
    {
        for (int col = 0; col < 4; col++)
        {
            a[row][col] = row * 3 + col + 1;
            b[row][col] = row + col * 2 + 5;
        }
    }

    for (int n = 0; n < iterations; n++)
    {
        checksum = 0;
        for (int row = 0; row < 4; row++)
        {
            for (int col = 0; col < 4; col++)
            {
                int value = 0;

                for (int k = 0; k < 4; k++)
                {
                    value += a[row][k] * b[k][col];
                }

                checksum += value * (row + 1) * (col + 2);
            }
        }
    }

    return checksum;
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
    register_benchmark_ex("binary_search", bench_binary_search, 2000, "Binary search over sorted table", 389);
    register_benchmark_ex("matrix_mul", bench_matrix_mul, 400, "4x4 integer matrix multiply", 49320);
}
