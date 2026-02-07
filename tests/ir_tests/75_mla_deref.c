/* Test MLA (Multiply-Accumulate) with dereferenced operands
 * 
 * This test verifies that the MLA optimization works when
 * MUL operands require memory dereferences, like in:
 *   sum += a[i] * b[i];
 * 
 * Expected: The compiler should fuse MUL + ADD into MLA
 * even when the operands are loaded from memory.
 */

int test_mla_deref(int *a, int *b, int acc) {
    return acc + (*a) * (*b);
}

int test_dot_product(int *a, int *b, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

int test_mixed(int *a, int b, int acc) {
    return acc + (*a) * b;  /* Only one DEREF */
}

int main(void) {
    int a[] = {1, 2, 3, 4, 5};
    int b[] = {1, 1, 1, 1, 1};
    int result;
    
    /* Test 1: Basic MLA with two dereferences */
    result = test_mla_deref(&a[0], &b[0], 10);
    if (result != 11) {
        return 1;  /* 10 + (1 * 1) = 11 */
    }
    
    /* Test 2: Loop with array access (dot product) */
    result = test_dot_product(a, b, 5);
    if (result != 15) {
        return 2;  /* 1+2+3+4+5 = 15 */
    }
    
    /* Test 3: Mixed - one DEREF and one register */
    result = test_mixed(&a[2], 3, 5);
    if (result != 14) {
        return 3;  /* 5 + (3 * 3) = 14 */
    }
    
    /* Test 4: Edge case with zero */
    int zero = 0;
    result = test_mla_deref(&zero, &zero, 100);
    if (result != 100) {
        return 4;  /* 100 + (0 * 0) = 100 */
    }
    
    /* Test 5: Negative values */
    int neg_a[] = {-1, -2, -3};
    int neg_b[] = {2, 3, 4};
    result = test_dot_product(neg_a, neg_b, 3);
    if (result != -20) {
        return 5;  /* (-1*2) + (-2*3) + (-3*4) = -2 - 6 - 12 = -20 */
    }
    
    return 0;
}
