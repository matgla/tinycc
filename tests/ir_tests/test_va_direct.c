#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

// Direct test: is the double in the right registers when passed?
// In soft-float AAPCS, 64-bit is passed in r0:r1 or r2:r3 (aligned to even register pair)

void test_double_pass(double d) {
    uint32_t *p = (uint32_t *)&d;
    printf("test_double_pass: lo=0x%08x hi=0x%08x val=%f\n", p[0], p[1], d);
}

int main() {
    double x = 2.6;
    printf("main: x=%f\n", x);
    test_double_pass(x);
    test_double_pass(2.6);
    return 0;
}
