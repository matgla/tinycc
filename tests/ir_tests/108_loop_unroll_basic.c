/* Test basic loop unrolling - small constant-trip-count loop */
#include <stdio.h>

int main() {
    int sum = 0;

    /* Simple loop: 5 iterations, 1 body instruction */
    for (int i = 0; i < 5; i++) {
        sum += 3;
    }

    printf("sum = %d\n", sum);
    if (sum != 15) {
        printf("FAIL: expected 15\n");
        return 1;
    }

    /* Loop with variable accumulation */
    int product = 1;
    for (int i = 0; i < 4; i++) {
        product *= 2;
    }

    printf("product = %d\n", product);
    if (product != 16) {
        printf("FAIL: expected 16\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
