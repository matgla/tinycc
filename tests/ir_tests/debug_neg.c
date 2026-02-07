/* Debug test for negative numbers */
#include <stdio.h>

int identity(int x) {
    return x;
}

int main() {
    printf("identity(-1): %d\n", identity(-1));
    printf("identity(-2): %d\n", identity(-2));
    printf("identity(-3): %d\n", identity(-3));
    printf("identity(-4): %d\n", identity(-4));
    printf("identity(-5): %d\n", identity(-5));
    printf("identity(-128): %d\n", identity(-128));
    printf("identity(-129): %d\n", identity(-129));
    printf("identity(-256): %d\n", identity(-256));
    return 0;
}
