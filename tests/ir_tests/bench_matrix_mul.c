#include <stdio.h>

int bench_matrix_mul(void)
{
    int a[4][4];
    int b[4][4];
    int checksum = 0;
    int iterations = 400;

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            a[row][col] = row * 3 + col + 1;
            b[row][col] = row + col * 2 + 5;
        }
    }

    for (int n = 0; n < iterations; n++) {
        checksum = 0;
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 4; col++) {
                int value = 0;

                for (int k = 0; k < 4; k++) {
                    value += a[row][k] * b[k][col];
                }

                checksum += value * (row + 1) * (col + 2);
            }
        }
    }

    return checksum;
}

int main(void)
{
    printf("matrix_mul: %d\n", bench_matrix_mul());
    return 0;
}
