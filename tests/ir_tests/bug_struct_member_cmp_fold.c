/*
 * Bug: identity comparison folding eliminates struct member comparisons.
 *
 * When two fields of the same global/extern struct are compared
 * (e.g. state.count >= state.size), both CMP operands are symbol
 * reference IROperands with the same vreg but different addends
 * (struct field offsets).  The identity comparison fold in
 * tcc_ir_opt_const_prop() only checked irop_get_vreg() equality,
 * ignoring the distinct addends, and incorrectly folded the
 * comparison as always-true — eliminating the branch entirely.
 *
 * Result: the reallocation guard was never tested, causing the
 * buffer to grow unconditionally on every call (memory explosion).
 *
 * Fix: check IRPoolSymref sym and addend when either CMP operand
 * has is_sym=1.  Different addends mean different memory locations.
 */

#include <stdio.h>

typedef struct {
    int size;     /* capacity */
    int count;    /* used entries */
} PoolState;

/* Global struct — accessed via GOT in PIC mode, triggering the bug.
 * The key is that both .count and .size are fields of the same global
 * struct at different offsets. */
PoolState pool = { 0, 0 };

static int grow_count = 0;

static void pool_init(void)
{
    pool.size = 4;
    pool.count = 0;
    grow_count = 0;
}

static void pool_allocate(void)
{
    /* This comparison was eliminated by the buggy optimizer:
     * both pool.count and pool.size got the same vreg, so the
     * identity fold treated them as equal and folded the branch
     * as always-true (>=), making the grow path unconditional. */
    if (pool.count >= pool.size) {
        pool.size = pool.size * 2;
        grow_count++;
    }
    pool.count++;
}

int main(void)
{
    int errors = 0;
    int i;

    pool_init();

    /* Allocate 4 entries — should fit without any grow */
    for (i = 0; i < 4; i++) {
        pool_allocate();
    }

    if (grow_count != 0) {
        printf("FAIL: expected 0 grows for first 4 entries, got %d\n",
               grow_count);
        errors++;
    } else {
        printf("PASS: no grow for first 4 entries\n");
    }

    /* 5th allocation should trigger exactly 1 grow (4 -> 8) */
    pool_allocate();

    if (grow_count != 1) {
        printf("FAIL: expected 1 grow after 5th entry, got %d\n",
               grow_count);
        errors++;
    } else {
        printf("PASS: exactly 1 grow after 5th entry\n");
    }

    if (pool.count != 5 || pool.size != 8) {
        printf("FAIL: pool.count=%d (expected 5), pool.size=%d (expected 8)\n",
               pool.count, pool.size);
        errors++;
    } else {
        printf("PASS: pool state correct (count=5, size=8)\n");
    }

    /* Allocate 3 more (6,7,8) — should fit without grow */
    for (i = 0; i < 3; i++) {
        pool_allocate();
    }

    if (grow_count != 1) {
        printf("FAIL: expected 1 grow after 8 entries, got %d\n",
               grow_count);
        errors++;
    } else {
        printf("PASS: still 1 grow after 8 entries\n");
    }

    /* 9th allocation should trigger grow #2 (8 -> 16) */
    pool_allocate();

    if (grow_count != 2) {
        printf("FAIL: expected 2 grows after 9th entry, got %d\n",
               grow_count);
        errors++;
    } else {
        printf("PASS: exactly 2 grows after 9th entry\n");
    }

    if (errors == 0) {
        printf("All struct member comparison tests passed!\n");
    } else {
        printf("%d test(s) failed!\n", errors);
    }
    return errors;
}
