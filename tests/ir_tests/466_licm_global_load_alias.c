/* licm_global_load: hoisting a loop-invariant global read out of a loop that
 * writes a DIFFERENT global.
 *
 * The gate used to be "does the loop write any global at all", so
 * `for (i = 0; i < N; i++) tbl[i] = len;` reloaded `len` from memory on every
 * iteration -- mibench_stringsearch's init_search spent 614,400 instructions
 * on that one load.  A store whose address is `&G + <variable>` cannot reach
 * another object (getting there through pointer arithmetic on G is undefined),
 * so a read of a different global is invariant across it.
 *
 * What has to be proved is the opposite direction: every shape where the read
 * is NOT invariant must still reload.  Those are the interesting cases here,
 * and each is written so a wrong hoist changes the printed value rather than
 * crashing:
 *
 *   - the loop writes the very global being read;
 *   - it writes through a pointer the pass cannot place;
 *   - it calls something that could write anything;
 *   - the global is volatile;
 *   - the store's offset is a CONSTANT, which is exactly what
 *     global_base_share emits when it re-bases a store to one global onto a
 *     neighbour's address -- after that rewrite two names denote one location
 *     (see 459_global_base_share_alias.c), so a constant offset must never be
 *     taken as proof of which object is written.
 */
#include <stdio.h>

#define N 24

static unsigned g_len;
static unsigned g_tbl[N];
static unsigned g_other;
static volatile unsigned g_vol;
unsigned g_pair_a, g_pair_b; /* neighbours, for the base-share shape */

volatile int vzero = 0;

static void sink(unsigned v) { g_other += v; }

/* The motivating shape: writes g_tbl, reads g_len.  g_len is invariant. */
static unsigned fill_from_other(unsigned seed)
{
    unsigned acc = 0;
    g_len = seed;
    for (unsigned i = 0; i < N; i++)
        g_tbl[i] = g_len + i;
    for (unsigned i = 0; i < N; i++)
        acc = acc * 3 + g_tbl[i];
    return acc;
}

/* NEGATIVE: the loop writes the global it reads, so every iteration must see
 * the previous one's value. */
static unsigned writes_what_it_reads(unsigned seed)
{
    g_len = seed;
    for (unsigned i = 0; i < N; i++)
    {
        g_tbl[i] = g_len;
        g_len = g_len + 1;
    }
    unsigned acc = 0;
    for (unsigned i = 0; i < N; i++)
        acc = acc * 3 + g_tbl[i];
    return acc * 7 + g_len;
}

/* NEGATIVE: the store goes through a pointer the pass cannot place -- here it
 * really does point at the global being read. */
static unsigned writes_through_pointer(unsigned seed, unsigned *p)
{
    g_len = seed;
    unsigned acc = 0;
    for (unsigned i = 0; i < N; i++)
    {
        acc = acc * 3 + g_len;
        *p = g_len + 1;
    }
    return acc;
}

/* NEGATIVE: a call that can write anything. */
static unsigned writes_via_call(unsigned seed)
{
    g_len = seed;
    unsigned acc = 0;
    for (unsigned i = 0; i < N; i++)
    {
        acc = acc * 3 + g_len;
        sink(i);
        if (i == 5)
            g_len = 99;
    }
    return acc;
}

/* NEGATIVE: a volatile read must reach memory every iteration. */
static unsigned reads_volatile(unsigned seed)
{
    g_vol = seed;
    unsigned acc = 0;
    for (unsigned i = 0; i < N; i++)
    {
        acc = acc * 3 + g_vol;
        g_tbl[i] = i;
    }
    return acc;
}

/* NEGATIVE-ish: constant-offset stores to adjacent globals, the shape
 * global_base_share re-bases.  The read of g_len must survive it correctly
 * whether or not the rewrite fires. */
static unsigned const_offset_neighbours(unsigned seed)
{
    g_len = seed;
    unsigned acc = 0;
    for (unsigned i = 0; i < N; i++)
    {
        g_pair_a = i;
        g_pair_b = i + 1;
        acc = acc * 3 + g_len + g_pair_a + g_pair_b;
    }
    return acc;
}

/* The read is invariant but the loop may exit before it -- hoisting a LOAD
 * into the preheader must not fault or change the value when the trip count
 * is zero. */
static unsigned maybe_zero_trip(unsigned seed, unsigned n)
{
    g_len = seed;
    unsigned acc = 1;
    for (unsigned i = 0; i < n; i++)
        acc = acc * 5 + g_len;
    return acc;
}

int main(void)
{
    unsigned s;
    for (s = 1; s <= 4; s++)
    {
        unsigned seed = s + (unsigned)vzero;
        printf("fill      %u -> %u\n", seed, fill_from_other(seed));
        printf("wwir      %u -> %u\n", seed, writes_what_it_reads(seed));
        printf("wtp       %u -> %u\n", seed, writes_through_pointer(seed, &g_len));
        printf("wvc       %u -> %u\n", seed, writes_via_call(seed));
        printf("vol       %u -> %u\n", seed, reads_volatile(seed));
        printf("neighbour %u -> %u\n", seed, const_offset_neighbours(seed));
        printf("zerotrip  %u -> %u %u\n", seed, maybe_zero_trip(seed, 0),
               maybe_zero_trip(seed, 3));
    }
    printf("other %u pair %u %u\n", g_other, g_pair_a, g_pair_b);
    return 0;
}
