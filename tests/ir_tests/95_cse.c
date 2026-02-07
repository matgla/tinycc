/* Test Common Subexpression Elimination
 * Repeated computations should be reused
 */
#include <stdio.h>

int test_arithmetic_cse(int a, int b) {
    /* (a + b) computed twice - should reuse */
    int x = a + b;
    int y = a + b;
    return x + y;
}

int test_complex_cse(int *arr, int idx) {
    /* arr[idx] pattern - index computation should be reused */
    int val1 = arr[idx];
    int val2 = arr[idx + 1];
    return val1 + val2;
}

int test_mul_cse(int a, int b, int c) {
    /* Multiple uses of a*b */
    int x = a * b;
    int y = a * b + c;
    int z = a * b - c;
    return x + y + z;
}

int main() {
    printf("test_arithmetic_cse(3, 4): %d\n", test_arithmetic_cse(3, 4));

    int arr[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    printf("test_complex_cse(arr, 1): %d\n", test_complex_cse(arr, 1));

    printf("test_mul_cse(2, 3, 1): %d\n", test_mul_cse(2, 3, 1));
    return 0;
}
