#include <stdio.h>

int main(void)
{
    double result_d;
    float result_f;
    
    /* Test __builtin_copysign for double */
    result_d = __builtin_copysign(3.14, -1.0);
    printf("copysign(3.14, -1.0) = %f\n", result_d);
    
    result_d = __builtin_copysign(-3.14, 1.0);
    printf("copysign(-3.14, 1.0) = %f\n", result_d);
    
    result_d = __builtin_copysign(2.5, 2.5);
    printf("copysign(2.5, 2.5) = %f\n", result_d);
    
    result_d = __builtin_copysign(-2.5, -2.5);
    printf("copysign(-2.5, -2.5) = %f\n", result_d);
    
    /* Test with zero */
    result_d = __builtin_copysign(1.0, -0.0);
    printf("copysign(1.0, -0.0) = %f\n", result_d);
    
    /* Test __builtin_copysignf for float */
    result_f = __builtin_copysignf(1.5f, -2.0f);
    printf("copysignf(1.5, -2.0) = %f\n", result_f);
    
    result_f = __builtin_copysignf(-1.5f, 2.0f);
    printf("copysignf(-1.5, 2.0) = %f\n", result_f);
    
    result_f = __builtin_copysignf(3.0f, 3.0f);
    printf("copysignf(3.0, 3.0) = %f\n", result_f);
    
    result_f = __builtin_copysignf(-3.0f, -3.0f);
    printf("copysignf(-3.0, -3.0) = %f\n", result_f);
    
    return 0;
}
