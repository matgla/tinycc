/* Debug test to narrow down the chain issue */
#include <stdio.h>

int test_mul_one(int x) {
    return x * 1;
}

int test_add_zero(int x) {
    return x + 0;
}

int test_chain_step1(int x) {
    int a = x + 0;  /* Should be x */
    return a;
}

int test_chain_step2(int x) {
    int a = x + 0;  /* Should be x */
    int b = a * 1;  /* Should be a (which is x) */
    return b;
}

int test_chain_full(int x) {
    return ((x + 0) * 1) + 0;  /* Should simplify to just x */
}

int main() {
    printf("test_mul_one(-5): %d\n", test_mul_one(-5));
    printf("test_add_zero(-5): %d\n", test_add_zero(-5));
    printf("test_chain_step1(-5): %d\n", test_chain_step1(-5));
    printf("test_chain_step2(-5): %d\n", test_chain_step2(-5));
    printf("test_chain_full(-5): %d\n", test_chain_full(-5));
    return 0;
}
