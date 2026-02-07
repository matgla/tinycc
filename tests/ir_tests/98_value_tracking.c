/* Test value tracking through arithmetic (Phase 2)
 *
 * This test verifies that constant values are tracked through
 * arithmetic operations (ADD, SUB) to enable comparison folding.
 */

#include <stdio.h>

/* Test value tracking through SUB
 * x = 1234 - 42 = 1192
 * The comparisons should be folded.
 */
int test_value_track_sub() {
    int x = 1234;
    x = x - 42;  /* x = 1192, should be tracked */
    if (x > 1000000) return 1;  /* Always false (1192 > 1000000 is false) */
    if (x < -1000000) return 2; /* Always false (1192 < -1000000 is false) */
    return x;  /* Should return 1192 */
}

/* Test value tracking through ADD */
int test_value_track_add() {
    int x = 100;
    x = x + 50;  /* x = 150 */
    if (x > 200) return 1;  /* Always false (150 > 200 is false) */
    if (x < 0) return 2;    /* Always false (150 < 0 is false) */
    return x;  /* Should return 150 */
}

/* Test chained arithmetic tracking */
int test_chained_arithmetic() {
    int x = 1000;
    x = x + 200;   /* x = 1200 */
    x = x - 100;   /* x = 1100 */
    x = x + 92;    /* x = 1192 */
    if (x != 1192) return 1;  /* Always false */
    return x;  /* Should return 1192 */
}

/* Test the conditionals benchmark pattern */
int test_conditionals_pattern(int iterations) {
    int r = 0;
    int n = 0;
    
    while (n < iterations) {
        /* These are loop-invariant and should be recognized */
        int i = 42;
        r = 1234;
        if (i & 1) {         /* Always false */
            r += 126;
        }
        /* r = 1234 - 42 = 1192 */
        r = r - 42;
        if (r > 1000000) {   /* Always false (1192 > 1000000 is false) */
            r = r >> 3;
        }
        if (r < -1000000) {  /* Always false (1192 < -1000000 is false) */
            r = -r;
        }
        n++;
    }
    
    return r;  /* Should be 1192 for any iterations >= 1 */
}

int main(void) {
    int errors = 0;
    
    int result1 = test_value_track_sub();
    if (result1 != 1192) {
        printf("FAIL: test_value_track_sub returned %d, expected 1192\n", result1);
        errors++;
    } else {
        printf("PASS: test_value_track_sub\n");
    }
    
    int result2 = test_value_track_add();
    if (result2 != 150) {
        printf("FAIL: test_value_track_add returned %d, expected 150\n", result2);
        errors++;
    } else {
        printf("PASS: test_value_track_add\n");
    }
    
    int result3 = test_chained_arithmetic();
    if (result3 != 1192) {
        printf("FAIL: test_chained_arithmetic returned %d, expected 1192\n", result3);
        errors++;
    } else {
        printf("PASS: test_chained_arithmetic\n");
    }
    
    int result4 = test_conditionals_pattern(5);
    if (result4 != 1192) {
        printf("FAIL: test_conditionals_pattern(5) returned %d, expected 1192\n", result4);
        errors++;
    } else {
        printf("PASS: test_conditionals_pattern\n");
    }
    
    if (errors == 0) {
        printf("All value tracking tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed!\n", errors);
        return 1;
    }
}
