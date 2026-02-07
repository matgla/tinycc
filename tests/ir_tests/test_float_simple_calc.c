/*
 * Simple float calculation test
 * Tests individual operations to identify where the bug is
 */

#include <stdio.h>
#include <stdint.h>

int main(void) {
    /* Test 1: Simple multiplication */
    volatile float a = 1.5f;
    volatile float b = 1.0f;
    volatile float r = a * b;
    
    printf("Test 1: 1.5 * 1.0 = %f (expected 1.5)\n", r);
    if (r != 1.5f) {
        printf("FAIL: Test 1\n");
        return 1;
    }
    
    /* Test 2: Simple addition */
    a = 1.5f;
    b = 2.5f;
    r = a + b;
    printf("Test 2: 1.5 + 2.5 = %f (expected 4.0)\n", r);
    if (r != 4.0f) {
        printf("FAIL: Test 2\n");
        return 2;
    }
    
    /* Test 3: Multiply and add (fused) */
    a = 1.0f;
    b = 1.5f;
    float c = 2.5f;
    r = a * b + c;
    printf("Test 3: 1.0 * 1.5 + 2.5 = %f (expected 4.0)\n", r);
    if (r != 4.0f) {
        printf("FAIL: Test 3\n");
        return 3;
    }
    
    /* Test 4: Division */
    a = 3.7f;
    b = 2.35f;
    r = a / b;
    printf("Test 4: 3.7 / 2.35 = %f (expected ~1.574)\n", r);
    
    /* Test 5: Full sequence */
    volatile float result = 1.0f;
    float a1 = 1.5f;
    float b1 = 2.5f;
    
    result = result * a1 + b1;
    printf("Step 1: %f (expected 4.0)\n", result);
    
    result = result * 0.9f + 0.1f;
    printf("Step 2: %f (expected 3.7)\n", result);
    
    float denom = result * 0.5f + 0.5f;
    printf("Denom: %f (expected 2.35)\n", denom);
    
    result = result / denom + 1.0f;
    printf("Step 3: %f (expected ~2.574)\n", result);
    
    int final = (int)(result * 1000);
    printf("Final: %d (expected 2574)\n", final);
    
    return (final == 2574) ? 0 : 5;
}
