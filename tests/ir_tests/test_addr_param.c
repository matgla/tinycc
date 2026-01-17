#include <stdio.h>
#include <stdint.h>

void test(const char *fmt) {
    printf("fmt value = %p\n", (void*)fmt);
    printf("&fmt addr = %p\n", (void*)&fmt);
    uint32_t *p = (uint32_t*)&fmt;
    printf("*(&fmt) = %p\n", (void*)*p);
}

int main() {
    test("hello");
    return 0;
}
