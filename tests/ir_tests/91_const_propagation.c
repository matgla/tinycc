/* Test constant propagation optimization
 * The constant x=5 should be propagated and the expression folded
 */
#include <stdio.h>

int test_const() {
    int x = 5;
    return x * 2 + x;  /* Should fold to 15 */
}

int test_zero_identity() {
    int a = 0;
    int b = 10;
    return b + a;  /* Should fold to 10 (x + 0 = x) */
}

int test_mul_identity() {
    int a = 1;
    int b = 42;
    return b * a;  /* Should fold to 42 (x * 1 = x) */
}

int test_mul_zero() {
    int a = 0;
    int b = 100;
    return b * a;  /* Should fold to 0 (x * 0 = 0) */
}

int main() {
    printf("test_const: %d\n", test_const());
    printf("test_zero_identity: %d\n", test_zero_identity());
    printf("test_mul_identity: %d\n", test_mul_identity());
    printf("test_mul_zero: %d\n", test_mul_zero());
    return 0;
}
