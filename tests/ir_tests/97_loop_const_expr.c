/* Test loop-invariant constant expression hoisting (Phase 3)
 *
 * This test verifies that constant computations inside loops
 * are hoisted to the pre-header.
 */

#include <stdio.h>

/* Test basic constant hoisting from loop
 * The computation of 'y' should be hoisted out of the loop.
 */
int test_const_hoist(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        int x = 5;           /* Loop-invariant: always 5 */
        int y = x * 2;       /* Loop-invariant: always 10 */
        sum += y;            /* Only this varies per iteration */
    }
    return sum;  /* Should be n * 10 */
}

/* Test chained constant expressions */
int test_chained_hoist(int n) {
    int result = 0;
    for (int i = 0; i < n; i++) {
        int a = 100;
        int b = a + 50;      /* Should be hoisted: 150 */
        int c = b - 25;      /* Should be hoisted: 125 */
        result += c;
    }
    return result;  /* Should be n * 125 */
}

/* Test with conditionals benchmark pattern */
int test_conditionals_pattern(int iterations) {
    int r = 0;
    int n = 0;
    
    while (n < iterations) {
        int i = 42;          /* Loop-invariant */
        r = 1234;            /* Loop-invariant */
        if (i & 1) {         /* Always false */
            r += 126;
        }
        if (i % 3) {         /* Always false (42 % 3 = 0) */
            r ^= 42;
        } else {
            r -= 42;         /* Always executed: r = 1192 */
        }
        if (r > 1000000) {   /* Always false */
            r >>= 3;
        }
        if (r < -1000000) {  /* Always false */
            r = -r;
        }
        n++;
    }
    
    return r;  /* Should be 1192 */
}

int main(void) {
    int errors = 0;
    
    int result1 = test_const_hoist(10);
    if (result1 != 100) {
        printf("FAIL: test_const_hoist(10) returned %d, expected 100\n", result1);
        errors++;
    } else {
        printf("PASS: test_const_hoist\n");
    }
    
    int result2 = test_chained_hoist(8);
    if (result2 != 1000) {
        printf("FAIL: test_chained_hoist(8) returned %d, expected 1000\n", result2);
        errors++;
    } else {
        printf("PASS: test_chained_hoist\n");
    }
    
    int result3 = test_conditionals_pattern(5);
    if (result3 != 1192) {
        printf("FAIL: test_conditionals_pattern(5) returned %d, expected 1192\n", result3);
        errors++;
    } else {
        printf("PASS: test_conditionals_pattern\n");
    }
    
    if (errors == 0) {
        printf("All LICM constant expression tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed!\n", errors);
        return 1;
    }
}
