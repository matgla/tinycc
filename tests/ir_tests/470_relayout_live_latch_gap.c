/* loop_relayout must not delete a live instruction in the leading latch gap.
 *
 * loop_relayout permutes a loop's [trampoline, body-end] region into
 * body / latch / back-edge, and the driver in loop_rotate.c runs it to a
 * fixpoint.  The indices between the latch's first slot and the one the body
 * actually jumps to are treated as dead: they are never copied into the
 * permuted buffer, and relayout_map folds the whole span onto new_latch_start
 * ("control continues at the increment").
 *
 * That held only as long as the span really was NOPs.  A *previous* relayout
 * of the same loop leaves the body's early-exit block sitting between the
 * trampoline and the back edge, so the next iteration saw a live `return` in
 * that gap: it was dropped outright and every branch to it was silently
 * retargeted at the loop increment -- the function simply lost its early exit
 * and ran to the normal return instead.
 *
 * Reaching the second iteration needs the switch to lower to a dense dispatch,
 * which is why the case values below are contiguous: a handful of scattered
 * cases compiles to a compare chain and never produced the shape.  This is the
 * exact shape of tcc's own dce_var_liveness bail-out scan, whose loss made the
 * self-hosted compiler delete stores to variables captured by nested
 * functions.
 */
#include <stdio.h>

volatile int vzero = 0;

/* dce_var_liveness's shape: a dense switch inside a loop, matching cases
 * return, default continues. */
static int scan_dense(int *ops, int n)
{
    for (int i = 0; i < n; i++)
    {
        switch (ops[i])
        {
        case 71: case 72: case 73: case 74:
        case 75: case 76: case 77:
            return 0;
        default:
            break;
        }
    }
    return 1;
}

/* Same, with a value carried out of the loop so the early exit is observable
 * as more than a constant. */
static int scan_index(int *ops, int n)
{
    for (int i = 0; i < n; i++)
    {
        switch (ops[i])
        {
        case 10: case 11: case 12: case 13:
        case 14: case 15: case 16: case 17:
            return i * 100 + ops[i];
        default:
            break;
        }
    }
    return -1;
}

/* Two dense ranges reaching two different early exits, so a single wrong
 * remap cannot be masked by both arms agreeing. */
static int scan_two_exits(int *ops, int n)
{
    int acc = 0;
    for (int i = 0; i < n; i++)
    {
        switch (ops[i])
        {
        case 20: case 21: case 22: case 23:
        case 24: case 25: case 26: case 27:
            return acc * 10 + 1;
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            return acc * 10 + 2;
        default:
            acc += ops[i];
            break;
        }
    }
    return acc * 10 + 9;
}

/* The early exit sits behind a nested loop, so the body-end scan has more than
 * one jump into the latch range to choose from. */
static int scan_nested(int *ops, int n)
{
    int acc = 0;
    for (int i = 0; i < n; i++)
    {
        for (int j = 0; j < 2; j++)
            acc += ops[i] & 1;
        switch (ops[i])
        {
        case 50: case 51: case 52: case 53:
        case 54: case 55: case 56: case 57:
            return acc * 1000 + i;
        default:
            break;
        }
    }
    return acc;
}

int main(void)
{
    int none[6]  = {1, 2, 3, 4, 5, 6};
    int hit71[6] = {1, 2, 71, 4, 5, 6};
    int hit77[6] = {1, 2, 3, 4, 77, 6};
    int idx[6]   = {0, 1, 13, 3, 4, 5};
    int two_a[6] = {1, 2, 23, 4, 5, 6};
    int two_b[6] = {1, 2, 3, 44, 5, 6};
    int nest[6]  = {1, 3, 55, 4, 5, 6};

    for (int k = 0; k < 6; k++)
    {
        none[k] += vzero; hit71[k] += vzero; hit77[k] += vzero;
        idx[k] += vzero; two_a[k] += vzero; two_b[k] += vzero; nest[k] += vzero;
    }

    printf("dense none=%d hit71=%d hit77=%d\n",
           scan_dense(none, 6), scan_dense(hit71, 6), scan_dense(hit77, 6));
    printf("index none=%d hit=%d\n", scan_index(none, 6), scan_index(idx, 6));
    printf("two none=%d a=%d b=%d\n",
           scan_two_exits(none, 6), scan_two_exits(two_a, 6), scan_two_exits(two_b, 6));
    printf("nested none=%d hit=%d\n", scan_nested(none, 6), scan_nested(nest, 6));
    return 0;
}
