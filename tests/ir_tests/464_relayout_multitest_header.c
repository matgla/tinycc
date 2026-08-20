/* loop_relayout with a multi-test header.
 *
 * The frontend lays a `for` loop out as header / latch / body, bridged by two
 * unconditional jumps.  loop_relayout permutes that region into body / latch /
 * back-edge and both jumps disappear.  It used to insist the header was
 * exactly `CMP; JUMPIF; JUMP`, so any short-circuit condition
 * (`for (...; x && i < 32; ...)`) kept both trampolines -- three of the nine
 * instructions in mibench_bitcount's bit_shifter inner loop were jumps.
 *
 * The permutation is placement only, so what has to be proved is that control
 * flow still lands where it did.  Every case below is a header shape that
 * reaches the trampoline differently; the negatives (a test that branches INTO
 * the body region, an exit from the middle) must come out with the same values
 * as the positives.
 */
#include <stdio.h>

volatile int vzero = 0;
int garr[8] = {3, 1, 4, 1, 5, 9, 2, 6};

/* The shape from mibench_bitcount: two conjuncts, the first a zero test. */
static int bit_shifter(long x)
{
    int i, n;
    for (i = n = 0; x && (i < 32); ++i, x >>= 1)
        n += (int)(x & 1L);
    return n;
}

/* Three conjuncts, the last one a call. */
static int nonneg(int v) { return v >= 0; }

static int three_tests(int n, int cap)
{
    int i, s = 0;
    for (i = 0; i < n && s < cap && nonneg(i); i++)
        s += i * 2;
    return s * 10 + i;
}

/* Disjunction: `||` puts a JUMPIF into the header whose target is the BODY,
 * not the exit -- the one in-region header target the pass is allowed to keep. */
static int either(int n, int force)
{
    int i, s = 0;
    for (i = 0; force || i < n; i++)
    {
        s += i;
        if (i >= 6)
            break;
    }
    return s * 10 + i;
}

/* continue jumps to the latch, break leaves the region: both edges are inside
 * the permuted range and must be remapped, not just the trampolines. */
static int with_break_continue(int n)
{
    int i, s = 0;
    for (i = 0; i < n && s < 1000; i++)
    {
        if ((i & 1) == 0)
            continue;
        if (i == 13)
            break;
        s += i;
    }
    return s * 100 + i;
}

/* Nested, both levels multi-test, inner loop reads the outer IV. */
static int nested(int n)
{
    int i, j, s = 0;
    for (i = 0; i < n && s < 5000; i++)
        for (j = 0; j < i && s < 5000; j++)
            s += garr[j & 7] + i;
    return s;
}

/* A header conjunct that reads memory the body writes: the tests stay put, so
 * the value read must not change. */
static int header_reads_body_store(int n)
{
    int i, s = 0;
    int slot = 0;
    for (i = 0; i < n && slot < 40; i++)
    {
        slot += i;
        s += slot;
    }
    return s * 100 + slot;
}

/* while-shaped (no latch of its own) beside a for-shaped sibling, so the
 * back-edge scan cannot confuse the two. */
static int sibling_loops(int n)
{
    int i = 0, s = 0;
    while (i < n && s < 300)
    {
        s += i * 3;
        i++;
    }
    for (int k = 0; k < n && s < 900; k++)
        s += k;
    return s * 10 + i;
}

/* An early return out of the middle of the body. */
static int early_return(int n)
{
    int i, s = 0;
    for (i = 0; i < n && i < 20; i++)
    {
        s += i;
        if (s > 30)
            return s * 1000 + i;
    }
    return s;
}

int main(void)
{
    long seeds[5] = {0x12345678L, 0L, 1L, 0x7FFFFFFFL, 0x55555555L};
    for (int k = 0; k < 5; k++)
        printf("shift %d: %d\n", k, bit_shifter(seeds[k] + vzero));

    for (int n = 0; n <= 9; n += 3)
    {
        printf("n=%d: %d %d %d %d %d %d %d\n", n, three_tests(n + vzero, 40), either(n + vzero, vzero),
               with_break_continue(n * 2 + vzero), nested(n + vzero), header_reads_body_store(n + vzero),
               sibling_loops(n + vzero), early_return(n + vzero));
    }

    printf("either forced: %d\n", either(3, 1));
    printf("bc long: %d\n", with_break_continue(30));
    printf("early: %d\n", early_return(50));
    return 0;
}
