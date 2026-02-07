/* Test explicit else */
#include <stdio.h>

const char *get_str(int i) {
    if (i == 1) {
        return "HELLO";
    } else {
        return "WORLD";
    }
}

int main(void) {
    printf("i=0: %s\n", get_str(0));
    printf("i=1: %s\n", get_str(1));
    return 0;
}
