#include <stdio.h>

/*
 * This test documents a known limitation of __builtin_signbit:
 * 
 * GCC returns the raw float sign mask for runtime __builtin_signbitf values,
 * while runtime double and constant-folded cases are normalized to 1.
 * 
 * This test documents the mixed native behavior so TCC can match it.
 */

int main(void)
{
    float neg_zero_f = -0.0f;
    double neg_zero_d = -0.0;
    
    int r;
    
    /* GCC returns the raw sign mask for float runtime values. */
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
