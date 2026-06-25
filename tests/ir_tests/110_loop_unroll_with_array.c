/* Test loop unrolling with array access patterns */
#include <stdio.h>

int main() {
    int arr[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    int sum = 0;

    /* Sum first 6 elements - small trip count with array access */
    for (int i = 0; i < 6; i++) {
        sum += arr[i];
    }

    printf("sum = %d\n", sum);
    if (sum != 21) {
        printf("FAIL: expected 21\n");
        return 1;
    }

    /* Copy between arrays */
    int dst[4];
    for (int i = 0; i < 4; i++) {
        dst[i] = arr[i] * 2;
    }

    int check = dst[0] + dst[1] + dst[2] + dst[3];
    printf("copy check = %d\n", check);
    if (check != 20) {
        printf("FAIL: expected 20\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
