/* ssa:switch_fold -- a SWITCH_TABLE whose selector is a compile-time constant.
 *
 * Nothing looked at the selector before: SCCP propagated the constant into the
 * dispatch and stopped, so the switch survived to ir/regalloc.c and became
 * either a .rodata value table indexed by a constant or, inside a loop, a full
 * PC-relative indirect jump run every iteration.  The fold picks the arm at
 * compile time; what has to hold is that it picks the SAME arm the dispatch
 * would have, for every way a selector can miss the table.
 */
#include <stdio.h>

int sink;

static int dense(int i)
{
    int r = 1000;
    switch (i) {
    case 0: r += 1; break;
    case 1: r -= 1; break;
    case 2: r *= 2; break;
    case 3: r /= 2; break;
    case 4: r ^= 4; break;
    case 5: r &= 5; break;
    case 6: r |= 6; break;
    case 7: r = (r ^ 0xff) ^ 0xff; break;
    default: r = -1; break;
    }
    return r;
}

/* No default arm: an out-of-range selector must land after the switch. */
static int no_default(int i)
{
    int r = 500;
    switch (i) {
    case 0: r = 10; break;
    case 1: r = 11; break;
    case 2: r = 12; break;
    case 3: r = 13; break;
    case 4: r = 14; break;
    case 5: r = 15; break;
    }
    return r;
}

/* Arms that fall through into the next one. */
static int fallthrough(int i)
{
    int r = 0;
    switch (i) {
    case 0: r += 1;   /* fall through */
    case 1: r += 2;   /* fall through */
    case 2: r += 4; break;
    case 3: r += 8;   /* fall through */
    case 4: r += 16; break;
    default: r = 999; break;
    }
    return r;
}

/* Table whose min is not zero, and is negative. */
static int negative_min(int i)
{
    int r = 0;
    switch (i) {
    case -4: r = 40; break;
    case -3: r = 30; break;
    case -2: r = 20; break;
    case -1: r = 10; break;
    case 0:  r = 1; break;
    case 1:  r = 2; break;
    default: r = -7; break;
    }
    return r;
}

/* An arm with a side effect: the fold must keep the one it selects. */
static int arm_with_store(int i)
{
    sink = 0;
    switch (i) {
    case 0: sink = 100; break;
    case 1: sink = 101; break;
    case 2: sink = 102; break;
    case 3: sink = 103; break;
    default: sink = -1; break;
    }
    return sink;
}

/* The bench_switch shape: a constant switch inside a counting loop.  Folding
 * the dispatch is what lets ssa:dead_loop see a pure body and delete the loop. */
static int in_loop(int iterations)
{
    int r = 0;
    for (int n = 0; n < iterations; n++) {
        int i = 7;
        r = 1000;
        switch (i) {
        case 0: r += i + 1; break;
        case 1: r -= i; break;
        case 2: r += 1; break;
        case 3: r = r / 2 + 1; break;
        case 4: r ^= i; break;
        case 5: r &= (0xffff + i); break;
        case 6: r |= (i & 0x0f); break;
        case 7: r = (r ^ 0xff) ^ 0xff; break;
        default: r = 0; break;
        }
    }
    return r;
}

/* A runtime selector must still dispatch correctly. */
static int variable(int i)
{
    switch (i) {
    case 0: return 70;
    case 1: return 71;
    case 2: return 72;
    case 3: return 73;
    case 4: return 74;
    default: return -70;
    }
}

int main(void)
{
    for (int i = -5; i <= 9; i++)
        printf("%d %d %d %d %d %d\n", dense(i), no_default(i), fallthrough(i),
               negative_min(i), arm_with_store(i), variable(i));
    printf("%d %d %d\n", in_loop(0), in_loop(1), in_loop(1000));
    return 0;
}
