/* Test with assignment instead of direct return */
#include <stdio.h>

const char *get_str(int i) {
    const char *result;
    if (i == 1) {
        result = "HELLO";
    } else {
        result = "WORLD";
    }
    return result;
}

int main(void) {
    printf("i=0: %s\n", get_str(0));
    printf("i=1: %s\n", get_str(1));
    return 0;
}
