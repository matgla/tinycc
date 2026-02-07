/* Test pure function hoisting - strcmp in loop
 * strcmp() is a pure function - its result depends only on its arguments.
 * When both arguments are loop-invariant, the call should be hoisted.
 */
#include <stdio.h>
#include <string.h>

int main() {
    const char *a = "hello";
    const char *b = "world";
    int count = 0;
    
    /* strcmp(a, b) is loop-invariant - should be hoisted */
    for (int i = 0; i < 10; i++) {
        if (strcmp(a, b) < 0) {
            count++;
        }
    }
    
    printf("count = %d\n", count);
    printf("expected = %d\n", 10);  /* "hello" < "world", so all 10 iterations */
    
    if (count == 10) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    
    return 0;
}
