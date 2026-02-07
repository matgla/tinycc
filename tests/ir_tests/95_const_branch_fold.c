/* Test constant branch folding optimization
 * 
 * This test verifies that branches with constant conditions are folded
 * at compile time. The optimizer should:
 * 1. Fold constant modulo operations (42 % 3 = 0)
 * 2. Fold constant bitwise operations (42 & 1 = 0)
 * 3. Convert conditional jumps to unconditional when condition is known
 * 4. Eliminate dead code that becomes unreachable
 */

#include <stdio.h>

/* Simple constant modulo - should fold to return 0 */
int test_fold_modulo(void)
{
    int x = 42 % 3;
    return x;
}

/* Test branch folding with constant values
 * The conditions are all compile-time constants, so:
 * - if (42 & 1) is always false (42 & 1 = 0)
 * - if (42 % 3) is always false (42 % 3 = 0) 
 * The result should always be 1234 - 42 = 1192
 */
int test_fold_branch(void)
{
    int r = 1234;
    int i = 42;
    
    /* This branch is never taken because 42 & 1 = 0 */
    if (i & 1) {
        r += 126;  /* Dead code - should be eliminated */
    }
    
    /* This branch is always taken because 42 % 3 = 0 */
    if (i % 3) {
        r ^= 42;  /* Dead code - should be eliminated */
    } else {
        r -= 42;  /* Always executed */
    }
    
    return r;  /* Should be 1192 */
}

/* Test nested constant branches */
int test_nested_fold(void)
{
    int x = 10;
    
    /* Outer condition is true (1) */
    if (1) {
        /* Inner condition is false (0) */
        if (0) {
            x = 999;  /* Dead code */
        }
        x = 20;  /* Always executed */
    }
    
    return x;  /* Should be 20 */
}

int main(void)
{
    int errors = 0;
    
    int result1 = test_fold_modulo();
    if (result1 != 0) {
        printf("FAIL: test_fold_modulo returned %d, expected 0\n", result1);
        errors++;
    } else {
        printf("PASS: test_fold_modulo\n");
    }
    
    int result2 = test_fold_branch();
    if (result2 != 1192) {
        printf("FAIL: test_fold_branch returned %d, expected 1192\n", result2);
        errors++;
    } else {
        printf("PASS: test_fold_branch\n");
    }
    
    int result3 = test_nested_fold();
    if (result3 != 20) {
        printf("FAIL: test_nested_fold returned %d, expected 20\n", result3);
        errors++;
    } else {
        printf("PASS: test_nested_fold\n");
    }
    
    if (errors == 0) {
        printf("All constant branch folding tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed!\n", errors);
        return 1;
    }
}
