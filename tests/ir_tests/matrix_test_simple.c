#include <stdio.h>

int test(void)
{
    int a[4][4];
    int b[4][4];
    int checksum = 0;

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            a[row][col] = row * 3 + col + 1;
            b[row][col] = row + col * 2 + 5;
        }
    }

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int value = 0;
            for (int k = 0; k < 4; k++) {
                value += a[row][k] * b[k][col];
            }
            checksum += value * (row + 1) * (col + 2);
        }
    }

    return checksum;
}

int main(void)
{
    printf("result: %d\n", test());
    return 0;
}
