/* Minimal test for string literal return from if-else */
#include <stdio.h>

const char *get_str(int i) {
    if (i == 0) return "ZERO";
    if (i == 1) return "ONE";
    if (i == 2) return "TWO";
    return "OTHER";
}

int main(void) {
    printf("i=0: %s\n", get_str(0));
    printf("i=1: %s\n", get_str(1));
    printf("i=2: %s\n", get_str(2));
    printf("i=3: %s\n", get_str(3));
    return 0;
}
