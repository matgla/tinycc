/* Debug test for negative numbers - boundary */
#include <stdio.h>

int identity(int x) {
    return x;
}

int main() {
    printf("identity(-254): %d\n", identity(-254));
    printf("identity(-255): %d\n", identity(-255));
    printf("identity(-256): %d\n", identity(-256));
    printf("identity(-257): %d\n", identity(-257));
    printf("identity(-258): %d\n", identity(-258));
    printf("identity(-512): %d\n", identity(-512));
    printf("identity(-1000): %d\n", identity(-1000));
    return 0;
}
