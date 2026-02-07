/*
 * Test: Induction Variable Strength Reduction
 *
 * Tests that IV strength reduction optimization correctly transforms
 * array indexing from base + i*stride to pointer increment pattern.
 *
 * Expected O1 behavior:
 * - Original: T = i * 4; addr = arr + T;
 * - Optimized: ptr initialized to arr, then ptr += 4 each iteration
 */

#include <stdio.h>

/* Basic array sum - simplest IV pattern */
int array_sum(int *arr, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}

/* Array sum with different stride (short = 2 bytes) */
int short_array_sum(short *arr, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}

/* Array copy - both read and write patterns */
void array_copy(int *dst, int *src, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* Multiple arrays accessed with same IV */
int array_diff_sum(int *a, int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i] - b[i];
    }
    return sum;
}

/* Test IV with decrement (might not be optimized, but shouldn't crash) */
int array_sum_reverse(int *arr, int n) {
    int sum = 0;
    for (int i = n - 1; i >= 0; i--) {
        sum += arr[i];
    }
    return sum;
}

int main(void) {
    int arr1[] = {1, 2, 3, 4, 5};
    int arr2[] = {10, 20, 30, 40, 50};
    int arr3[5] = {0};
    short sarr[] = {100, 200, 300, 400, 500};

    printf("array_sum: %d\n", array_sum(arr1, 5));  /* Should be 15 */
    printf("short_array_sum: %d\n", short_array_sum(sarr, 5));  /* Should be 1500 */

    array_copy(arr3, arr1, 5);
    printf("array_copy: %d %d %d %d %d\n", arr3[0], arr3[1], arr3[2], arr3[3], arr3[4]);

    printf("array_diff_sum: %d\n", array_diff_sum(arr2, arr1, 5));  /* 10-1 + 20-2 + 30-3 + 40-4 + 50-5 = 135 */

    printf("array_sum_reverse: %d\n", array_sum_reverse(arr1, 5));  /* Should be 15 */

    printf("PASSED\n");
    return 0;
}
