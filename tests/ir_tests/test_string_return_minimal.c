/* Minimal test - single if return */
#include <stdio.h>

const char *get_str(int i) {
    if (i == 1) return "HELLO";
    return "WORLD";
}

int main(void) {
    printf("i=0: %s\n", get_str(0));
    printf("i=1: %s\n", get_str(1));
    printf("i=2: %s\n", get_str(2));
    return 0;
}
