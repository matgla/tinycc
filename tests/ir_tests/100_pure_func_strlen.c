/* Test pure function hoisting - strlen in loop
 * strlen() is a pure function - its result depends only on its argument.
 * When the argument is loop-invariant, the call should be hoisted.
 */
#include <stdio.h>
#include <string.h>

volatile int sink = 0;

int main() {
    const char *str = "hello";
    int sum = 0;
    
    /* strlen(str) is loop-invariant - should be hoisted */
    for (int i = 0; i < 5; i++) {
        sum += strlen(str);
    }
    
    printf("sum = %d\n", sum);
    printf("expected = %d\n", 25);  /* 5 * 5 = 25 */
    
    if (sum == 25) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    
    return 0;
}
