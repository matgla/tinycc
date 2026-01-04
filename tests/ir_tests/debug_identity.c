/* Debug test for identity function */
#include <stdio.h>

int identity(int x) {
    return x;
}

int main() {
    printf("identity(-5): %d\n", identity(-5));
    printf("identity(-7): %d\n", identity(-7));
    printf("identity(42): %d\n", identity(42));
    printf("identity(0): %d\n", identity(0));
    return 0;
}
