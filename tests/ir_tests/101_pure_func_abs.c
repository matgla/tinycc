/* Test pure function hoisting - abs in loop
 * abs() is a const function - its result depends only on its argument.
 * When the argument is loop-invariant, the call should be hoisted.
 */
#include <stdio.h>
#include <stdlib.h>

int main() {
    int x = -42;
    int sum = 0;
    
    /* abs(x) is loop-invariant - should be hoisted */
    for (int i = 0; i < 10; i++) {
        sum += abs(x);
    }
    
    printf("sum = %d\n", sum);
    printf("expected = %d\n", 420);  /* 10 * 42 = 420 */
    
    if (sum == 420) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    
    return 0;
}
