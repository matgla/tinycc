/* Test that loops which should NOT be unrolled still work correctly */
#include <stdio.h>

volatile int vol = 0;

int main() {
    int sum = 0;

    /* Trip count too large (> UNROLL_MAX_TRIP_COUNT=16) */
    for (int i = 0; i < 100; i++) {
        sum += 1;
    }
    printf("large trip: %d\n", sum);
    if (sum != 100) {
        printf("FAIL\n");
        return 1;
    }

    /* Loop with function call in body (not unrollable) */
    sum = 0;
    for (int i = 0; i < 5; i++) {
        sum += printf("");
    }
    /* printf("") returns 0, so sum should be 0 */
    printf("call in body: %d\n", sum);

    /* Nested loops - inner should not be unrolled */
    sum = 0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            sum += 1;
        }
    }
    printf("nested: %d\n", sum);
    if (sum != 12) {
        printf("FAIL\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
