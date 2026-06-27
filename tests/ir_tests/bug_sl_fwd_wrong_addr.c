#include <stdio.h>

int main(void)
{
    int arr[4] = {0, 0, 0, 1652065559};
    int x = 0;
    for (int i = 0; i < 5; i++) {
        x = arr[3];
        arr[2] = -2385;
    }
    printf("x=%d\n", x);
    return x != 1652065559;
}
