#include <stdio.h>

/*
 * This test documents a known limitation of __builtin_signbit:
 * 
 * The current implementation uses x < 0.0 comparison for runtime values,
 * which returns 0 for -0.0. However, according to IEEE 754 and GCC behavior,
 * signbit(-0.0) should return 1 (non-zero) because -0.0 has the sign bit set.
 * 
 * This limitation only affects runtime values. Compile-time constants
 * are handled correctly by extracting the sign bit from the raw representation.
 */

int main(void)
{
    float neg_zero_f = -0.0f;
    double neg_zero_d = -0.0;
    
    int r;
    
    /* These should return 1 (non-zero) according to IEEE 754, but return 0 */
    r = __builtin_signbitf(neg_zero_f);
    printf("signbitf(-0.0f) at runtime: %d (expected: 1)\n", r);
    
    r = __builtin_signbit(neg_zero_d);
    printf("signbit(-0.0) at runtime: %d (expected: 1)\n", r);
    
    /* Compile-time constants are handled correctly */
    r = __builtin_signbitf(-0.0f);
    printf("signbitf(-0.0f) const: %d (expected: 1)\n", r);
    
    r = __builtin_signbit(-0.0);
    printf("signbit(-0.0) const: %d (expected: 1)\n", r);
    
    return 0;
}
