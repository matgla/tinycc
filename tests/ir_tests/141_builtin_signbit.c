#include <stdio.h>

int main(void)
{
    float pos_f = 1.5f;
    float neg_f = -1.5f;
    float zero_f = 0.0f;
    float neg_zero_f = -0.0f;
    
    double pos_d = 2.5;
    double neg_d = -2.5;
    double zero_d = 0.0;
    double neg_zero_d = -0.0;
    
    int r;
    
    /* Test __builtin_signbitf for float */
    r = __builtin_signbitf(pos_f);
    printf("pos_f: %d\n", r);
    r = __builtin_signbitf(neg_f);
    printf("neg_f: %d\n", r);
    r = __builtin_signbitf(zero_f);
    printf("zero_f: %d\n", r);
    /* GCC returns the raw float sign mask for runtime __builtin_signbitf values. */
    r = __builtin_signbitf(neg_zero_f);
    printf("neg_zero_f: %d\n", r);
    
    /* Test __builtin_signbit for double */
    r = __builtin_signbit(pos_d);
    printf("pos_d: %d\n", r);
    r = __builtin_signbit(neg_d);
    printf("neg_d: %d\n", r);
    r = __builtin_signbit(zero_d);
    printf("zero_d: %d\n", r);
    r = __builtin_signbit(neg_zero_d);
    printf("neg_zero_d: %d\n", r);
    
    /* Test with constants */
    r = __builtin_signbitf(3.14f);
    printf("const pos: %d\n", r);
    r = __builtin_signbitf(-3.14f);
    printf("const neg f: %d\n", r);
    r = __builtin_signbit(3.14);
    printf("const pos d: %d\n", r);
    r = __builtin_signbit(-3.14);
    printf("const neg d: %d\n", r);
    
    return 0;
}
