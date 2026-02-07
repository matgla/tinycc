/* Test loop with invariant variable
 * The constant 'base' should be propagated into the loop
 */
#include <stdio.h>

void test_loop(int *arr, int size) {
    int base = 100;
    for (int i = 0; i < size; i++) {
        arr[i] = base + i;
    }
}

int main() {
    int arr[10];
    test_loop(arr, 10);

    for (int i = 0; i < 10; i++) {
        printf("arr[%d] = %d\n", i, arr[i]);
    }
    return 0;
}
