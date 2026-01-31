#include <stdio.h>

const char *get_world(void) {
    return "WORLD";
}

int main(void) {
    printf("result: %s\n", get_world());
    return 0;
}
