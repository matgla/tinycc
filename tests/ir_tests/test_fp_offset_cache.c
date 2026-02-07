/* Test Frame Pointer Offset Caching Optimization
 * 
 * This test verifies that multiple accesses to local array elements
 * reuse the same base address calculation (frame pointer + offset)
 * when the optimization is enabled.
 * 
 * Without optimization: Each arr[i] access recalculates &arr[0]
 * With optimization: &arr[0] is computed once and reused
 * 
 * BASELINE (before optimization):
 *   - test_loop_access(): 7 FP offset calculations, only 1 unique
 *   - test_swap_pattern(): 5 FP offset calculations, only 1 unique
 *   - Total: 12 calculations, 10 redundant (83.3% waste)
 * 
 * EXPECTED (after optimization):
 *   - ~83% reduction in FP offset calculations
 *   - Each unique offset computed only once per function
 */
#include <stdio.h>

/* Test 1: Multiple consecutive array accesses
 * These should share the base address calculation */
void test_multiple_access(void)
{
    int arr[64];
    
    /* Multiple stores to array - base should be computed once */
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    arr[3] = 40;
    arr[4] = 50;
    
    /* Multiple loads from array - base should be reused */
    volatile int sum = arr[0] + arr[1] + arr[2] + arr[3] + arr[4];
    
    printf("test_multiple_access: sum = %d\n", sum);
}

/* Test 2: Array access in loop - inner loop optimization
 * The array base should be computed outside the inner loop */
void test_loop_access(void)
{
    int arr[64];
    int i, j;
    
    /* Initialize array */
    for (i = 0; i < 64; i++) {
        arr[i] = i * 10;
    }
    
    /* Simple bubble sort-like pattern - multiple array accesses */
    for (i = 0; i < 10; i++) {
        for (j = 0; j < 9; j++) {
            if (arr[j] > arr[j + 1]) {
                int tmp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = tmp;
            }
        }
    }
    
    printf("test_loop_access: arr[0] = %d, arr[1] = %d\n", arr[0], arr[1]);
}

/* Test 3: Mixed reads and writes to same locations
 * Should still reuse base address */
void test_mixed_access(void)
{
    int arr[16];
    
    arr[0] = 1;
    arr[0] = arr[0] + 1;
    arr[0] = arr[0] * 2;
    arr[1] = arr[0] + arr[0];
    
    printf("test_mixed_access: arr[0] = %d, arr[1] = %d\n", arr[0], arr[1]);
}

/* Test 4: Array element swapping - multiple accesses per iteration */
void test_swap_pattern(void)
{
    int arr[8] = {80, 70, 60, 50, 40, 30, 20, 10};
    
    /* Swap adjacent elements - 4 accesses to array base per iteration */
    for (int i = 0; i < 7; i += 2) {
        int tmp = arr[i];
        arr[i] = arr[i + 1];
        arr[i + 1] = tmp;
    }
    
    printf("test_swap_pattern: %d %d %d %d\n", arr[0], arr[1], arr[2], arr[3]);
}

int main(void)
{
    printf("=== Frame Pointer Offset Cache Test ===\n");
    
    test_multiple_access();
    test_loop_access();
    test_mixed_access();
    test_swap_pattern();
    
    printf("=== Test Complete ===\n");
    return 0;
}
