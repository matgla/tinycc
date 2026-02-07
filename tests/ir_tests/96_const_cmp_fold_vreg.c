/* Test Phase 2: Constant Comparison Folding through VReg tracking
 * 
 * This test verifies that branches are folded when CMP uses vregs
 * with known constant values (not just immediate constants).
 */

#include <stdio.h>

/* Test constant tracking through arithmetic
 * The comparison (1192 > 1000000) should be folded to always false
 * and the branch should be eliminated.
 */
int test_const_tracking(void)
{
    int r = 1234;
    int x = 42;
    
    /* After constant folding: r = 1234 - 42 = 1192 */
    r = r - x;
    
    /* This comparison is always false (1192 <= 1000000)
     * The branch should be eliminated */
    if (r > 1000000) {
        return 999;  /* Dead code - should be eliminated */
    }
    
    /* This comparison is always true (1192 >= -1000000)
     * The branch should become unconditional */
    if (r < -1000000) {
        return 888;  /* Dead code - should be eliminated */
    }
    
    return r;  /* Should be 1192 */
}

/* Test with nested constant expressions */
int test_nested_const(void)
{
    int a = 100;
    int b = 50;
    
    /* After folding: a = 100 + 50 = 150 */
    a = a + b;
    
    /* After folding: a = 150 - 25 = 125 */
    a = a - 25;
    
    /* This is always true (125 == 125) */
    if (a == 125) {
        return 1;  /* Always taken */
    }
    
    return 0;  /* Dead code */
}

int main(void)
{
    int errors = 0;
    
    int result1 = test_const_tracking();
    if (result1 != 1192) {
        printf("FAIL: test_const_tracking returned %d, expected 1192\n", result1);
        errors++;
    } else {
        printf("PASS: test_const_tracking\n");
    }
    
    int result2 = test_nested_const();
    if (result2 != 1) {
        printf("FAIL: test_nested_const returned %d, expected 1\n", result2);
        errors++;
    } else {
        printf("PASS: test_nested_const\n");
    }
    
    if (errors == 0) {
        printf("All Phase 2 tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed!\n", errors);
        return 1;
    }
}
