/* Test pure function hoisting - multiple calls in same loop
 * Both strlen calls are loop-invariant and should be hoisted.
 */
#include <stdio.h>
#include <string.h>

int main() {
    const char *str1 = "hello";
    const char *str2 = "world!!!";
    int sum = 0;
    
    /* Both strlen calls are loop-invariant */
    for (int i = 0; i < 3; i++) {
        sum += strlen(str1) + strlen(str2);
    }
    
    printf("sum = %d\n", sum);
    printf("expected = %d\n", 39);  /* 3 * (5 + 8) = 39 */
    
    if (sum == 39) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    
    return 0;
}
