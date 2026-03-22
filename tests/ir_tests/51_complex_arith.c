#include <stdio.h>

/* Test complex arithmetic operations */

/* Complex addition: (a+bi) + (c+di) = (a+c) + (b+d)i */
_Complex float test_add(_Complex float a, _Complex float b)
{
    return a + b;
}

/* Complex subtraction */
_Complex float test_sub(_Complex float a, _Complex float b)
{
    return a - b;
}

/* Complex multiplication: (a+bi) * (c+di) = (ac-bd) + i(ad+bc) */
_Complex float test_mul(_Complex float a, _Complex float b)
{
    return a * b;
}

/* Complex division */
_Complex float test_div(_Complex float a, _Complex float b)
{
    return a / b;
}

/* Helper to print complex float - takes individual components */
void print_complex(const char *name, float real, float imag)
{
    printf("%s: %.1f + %.1fi\n", name, real, imag);
}

int main(void)
{
    /* Create complex values using real-to-complex conversion
     * When assigning a real to complex, imag part is 0 */
    _Complex float a = 1.0f;   /* 1 + 0i */
    _Complex float b = 3.0f;   /* 3 + 0i */
    _Complex float result;
    float real, imag;
    int pass = 1;
    
    /* Test addition: (1+0i) + (3+0i) = (4+0i) */
    result = test_add(a, b);
    real = __real__ result;
    imag = __imag__ result;
    print_complex("add", real, imag);
    if (real < 3.9f || real > 4.1f || imag < -0.1f || imag > 0.1f) {
        printf("FAIL: add expected 4.0 + 0.0i\n");
        pass = 0;
    }
    
    /* Test subtraction: (1+0i) - (3+0i) = (-2+0i) */
    result = test_sub(a, b);
    real = __real__ result;
    imag = __imag__ result;
    print_complex("sub", real, imag);
    if (real < -2.1f || real > -1.9f || imag < -0.1f || imag > 0.1f) {
        printf("FAIL: sub expected -2.0 + 0.0i\n");
        pass = 0;
    }
    
    /* Test multiplication: (1+0i) * (3+0i) = (3+0i) */
    result = test_mul(a, b);
    real = __real__ result;
    imag = __imag__ result;
    print_complex("mul", real, imag);
    if (real < 2.9f || real > 3.1f || imag < -0.1f || imag > 0.1f) {
        printf("FAIL: mul expected 3.0 + 0.0i\n");
        pass = 0;
    }
    
    /* Test division: (3+0i) / (1+0i) = (3+0i) */
    result = test_div(b, a);
    real = __real__ result;
    imag = __imag__ result;
    print_complex("div", real, imag);
    if (real < 2.9f || real > 3.1f || imag < -0.1f || imag > 0.1f) {
        printf("FAIL: div expected 3.0 + 0.0i\n");
        pass = 0;
    }
    
    if (pass) {
        printf("OK: All basic complex arithmetic tests passed!\n");
        return 0;
    } else {
        printf("FAIL: Some tests failed!\n");
        return 1;
    }
}
