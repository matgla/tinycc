/*
 * Debug test for TCC float math issue
 * Traces intermediate values to identify where computation diverges
 */

#include <stdio.h>

int main(void) {
    volatile float result = 1.0f;
    float a = 1.5f;
    float b = 2.5f;
    
    printf("Initial: result=%f a=%f b=%f\n", result, a, b);
    
    /* Step 1: result = result * a + b */
    result = result * a + b;
    printf("Step 1 (r*r+b): result=%f (expected ~4.0)\n", result);
    
    /* Step 2: result = result * 0.9f + 0.1f */
    result = result * 0.9f + 0.1f;
    printf("Step 2 (r*0.9+0.1): result=%f (expected ~3.7)\n", result);
    
    /* Step 3: result = result / (result * 0.5f + 0.5f) + 1.0f */
    float denom = result * 0.5f + 0.5f;
    printf("Step 3 denom: %f (expected ~2.35)\n", denom);
    result = result / denom + 1.0f;
    printf("Step 3 final: result=%f (expected ~2.574)\n", result);
    
    /* Step 4: a = result * 0.5f; b = result * 0.3f; */
    a = result * 0.5f;
    b = result * 0.3f;
    printf("Final a=%f b=%f\n", a, b);
    
    int final = (int)(result * 1000);
    printf("Final result * 1000 = %d (expected 2574)\n", final);
    
    return (final == 2574) ? 0 : 1;
}
