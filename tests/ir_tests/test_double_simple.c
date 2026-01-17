#include <stdio.h>
#include <stdint.h>

// In AAPCS soft-float, double is passed in r2:r3 (aligned to even pair)
// For printf("%f", x), r0=fmt, then double needs to be aligned to r2:r3

int main() {
    double x = 2.6;
    uint32_t *p = (uint32_t *)&x;
    printf("Raw bytes: lo=0x%08x hi=0x%08x\n", p[0], p[1]);
    
    // If TCC passes the double incorrectly, we'll see wrong output
    // The expected is r0=fmt, r1=unused, r2=lo, r3=hi
    printf("x=%f\n", x);
    return 0;
}
