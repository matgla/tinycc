/* Debug test for negative numbers - pattern */
#include <stdio.h>

int identity(int x) {
    return x;
}

int main() {
    printf("identity(1): %d\n", identity(1));
    printf("identity(2): %d\n", identity(2));
    printf("identity(127): %d\n", identity(127));
    printf("identity(128): %d\n", identity(128));
    printf("identity(255): %d\n", identity(255));
    printf("identity(256): %d\n", identity(256));
    printf("identity(257): %d\n", identity(257));
    printf("identity(1000): %d\n", identity(1000));
    return 0;
}
