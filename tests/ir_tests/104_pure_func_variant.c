/* Test pure function hoisting - variant argument (should NOT hoist)
 * strlen is called on a loop-variant pointer - should NOT be hoisted.
 */
#include <stdio.h>
#include <string.h>

int main() {
    const char *strings[] = {"a", "bb", "ccc", "dddd", "eeeee"};
    int sum = 0;
    
    /* strlen(strings[i]) is NOT loop-invariant - should NOT be hoisted */
    for (int i = 0; i < 5; i++) {
        sum += strlen(strings[i]);
    }
    
    printf("sum = %d\n", sum);
    printf("expected = %d\n", 15);  /* 1 + 2 + 3 + 4 + 5 = 15 */
    
    if (sum == 15) {
        printf("PASS\n");
    } else {
        printf("FAIL\n");
    }
    
    return 0;
}
