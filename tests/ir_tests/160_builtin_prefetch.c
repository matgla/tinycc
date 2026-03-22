/* Test __builtin_prefetch */
#include <stdio.h>

int main(void)
{
    int data[100];
    int i;
    
    /* Initialize array */
    for (i = 0; i < 100; i++) {
        data[i] = i;
    }
    
    /* Test basic prefetch - just the address (defaults to read, high locality) */
    __builtin_prefetch(&data[50]);
    
    /* Test prefetch with rw=0 (read) */
    __builtin_prefetch(&data[60], 0);
    
    /* Test prefetch with rw=1 (write) */
    __builtin_prefetch(&data[70], 1);
    
    /* Test prefetch with rw and locality (0-3) */
    __builtin_prefetch(&data[80], 0, 3);
    __builtin_prefetch(&data[90], 1, 0);
    
    /* Use the data to make sure prefetch didn't break anything */
    int sum = 0;
    for (i = 0; i < 100; i++) {
        sum += data[i];
    }
    
    /* Verify sum is correct (0+1+2+...+99 = 4950) */
    if (sum != 4950) {
        printf("FAIL: Sum mismatch, expected 4950, got %d\n", sum);
        return 1;
    }
    
    printf("PASS: __builtin_prefetch works correctly\n");
    return 0;
}
