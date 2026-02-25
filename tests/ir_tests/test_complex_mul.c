#include <stdio.h>

_Complex float test_mul(_Complex float a, _Complex float b) {
    return a * b;
}

int main(void) {
    _Complex float x = 2.0f;   /* 2 + 0i */
    _Complex float y = 3.0f;   /* 3 + 0i */
    _Complex float z = test_mul(x, y);  /* 6 + 0i */
    
    float real = __real__ z;
    float imag = __imag__ z;
    
    printf("mul: %.1f + %.1fi\n", real, imag);
    
    if (real > 5.9f && real < 6.1f && imag > -0.1f && imag < 0.1f) {
        printf("OK: Multiplication works!\n");
        return 0;
    } else {
        printf("FAIL: Expected 6.0 + 0.0i\n");
        return 1;
    }
}
