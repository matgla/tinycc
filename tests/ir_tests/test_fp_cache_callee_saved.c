/* Test FP cache with callee-saved registers
 * This test has high register pressure to force use of r4-r11
 */
#include <stdio.h>

void test_high_pressure(void)
{
    /* Many local variables to exhaust caller-saved registers */
    int arr[64];
    int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    int i = 9, j = 10, k = 11, l = 12, m = 13, n = 14, o = 15, p = 16;
    
    /* Use all variables to prevent elimination */
    volatile int sum = a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p;
    
    /* Multiple array accesses - should use callee-saved regs for addresses */
    arr[0] = sum;
    arr[1] = sum + 1;
    arr[2] = sum + 2;
    arr[3] = sum + 3;
    arr[4] = sum + 4;
    arr[5] = sum + 5;
    arr[6] = sum + 6;
    arr[7] = sum + 7;
    
    /* Read back */
    volatile int result = arr[0] + arr[1] + arr[2] + arr[3] + 
                          arr[4] + arr[5] + arr[6] + arr[7];
    
    printf("Result: %d\n", result);
}

int main(void)
{
    test_high_pressure();
    return 0;
}
