/* Test chained arithmetic with identity operations
 * All identity operations should be eliminated
 */
#include <stdio.h>

int test_chain(int x) {
    return ((x + 0) * 1) + 0;  /* Should simplify to just x */
}

int test_shift_zero(int x) {
    return (x << 0) >> 0;  /* Should simplify to x */
}

int test_and_or_identity(int x) {
    int a = x | 0;      /* x | 0 = x */
    int b = a & -1;     /* x & -1 = x (all bits set) */
    return b;
}

int test_sub_zero(int x) {
    return x - 0;  /* Should simplify to x */
}

int main() {
    printf("test_chain(42): %d\n", test_chain(42));
    printf("test_chain(0): %d\n", test_chain(0));
    printf("test_chain(-5): %d\n", test_chain(-5));

    printf("test_shift_zero(123): %d\n", test_shift_zero(123));
    printf("test_and_or_identity(255): %d\n", test_and_or_identity(255));
    printf("test_sub_zero(99): %d\n", test_sub_zero(99));
    return 0;
}
