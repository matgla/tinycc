/* Exercise the rotated (bottom-test) arm of ssa_opt_dead_loop.c.
 *
 * A loop that comes out of rotation is "guard + body + latch CMP/JUMPIF back",
 * with no CMP at its header -- so analyze_loop_entry never matched it and the
 * empty counting shell survived to codegen.  The rotated arm kills the
 * back-edge, but only when the body is pure, the trip count is finite and
 * every value read after the loop is a compile-time constant.  Each negative
 * below trips one of those conditions and must keep counting; the values are
 * what proves it, so every case is exercised at a zero and a nonzero bound.
 */
#include <stdio.h>

int garr[8] = {3, 1, 4, 1, 5, 9, 2, 6};
int sink;
volatile int vsink;

static int const_result(int n)
{
    int r = 7;
    for (int i = 0; i < n; i++)
        r = 91967;
    return r;
}

/* No closed form for the recurrence, so nothing upstream evaluates it away and
 * the exit value genuinely depends on the trip count. */
static int mangle(int n)
{
    int r = 1;
    for (int i = 0; i < n; i++)
        r = r * 3 + 1;
    return r;
}

/* The counter itself is read after the loop. */
static int counter_escapes(int n)
{
    int i;
    for (i = 0; i < n; i++)
        ;
    return i;
}

/* A store is a side effect. */
static int store_each(int n)
{
    sink = -1;
    for (int i = 0; i < n; i++)
        sink = i;
    return sink;
}

/* Counting down toward the bound terminates just as well. */
static int down_counting(int n)
{
    int r = 5;
    for (int i = n; i > 0; i--)
        r = 1234;
    return r;
}

static unsigned unsigned_bound(unsigned n)
{
    unsigned r = 1;
    for (unsigned i = 0; i < n; i++)
        r = 0xabcdu;
    return r;
}

/* Steps other than 1 are fine as long as they head toward the bound. */
static int step_two(int n)
{
    int r = 0;
    for (int i = 0; i < n; i += 2)
        r = 55;
    return r;
}

/* Unsigned counter that starts near the top of the range: it still reaches the
 * bound before it can wrap, so deleting the loop is sound. */
static unsigned near_max(unsigned n)
{
    unsigned r = 0;
    for (unsigned i = 0xfffffff0u; i < n; i++)
        r = 0x1234u;
    return r;
}

/* A break gives the header a second exit and carries out a varying value. */
static int with_break(int n)
{
    int r = 0;
    for (int i = 0; i < n; i++) {
        r = i * 2;
        if (i == 3)
            break;
    }
    return r;
}

/* A continue gives the header a second back-edge. */
static int with_continue(int n)
{
    int r = 0;
    for (int i = 0; i < n; i++) {
        if (i & 1)
            continue;
        r = 77;
    }
    return r;
}

/* The bound is rewritten inside the loop, so it is not a fixed target. */
static int shrinking_bound(int n)
{
    int r = 0;
    int b = n;
    for (int i = 0; i < b; i++) {
        r = 66;
        b--;
    }
    return r;
}

/* Pure loads: the loop may go, but only if the escaping value is constant. */
static int load_only(int n)
{
    int r = -1;
    for (int i = 0; i < n; i++)
        r = garr[3];
    return r;
}

static int load_varying(int n)
{
    int r = -1;
    for (int i = 0; i < n && i < 8; i++)
        r = garr[i];
    return r;
}

/* Bottom-tested with no entry guard: the body always runs once. */
static int do_while(int n)
{
    int r = 0;
    int i = 0;
    do {
        r = 88;
        i++;
    } while (i < n);
    return r;
}

/* No induction variable at all. */
static int no_iv(int n)
{
    int r = 0;
    int i = 0;
    for (;;) {
        r = 99;
        if (i >= n)
            break;
        i++;
    }
    return r;
}

/* Inner loop is dead; the outer one feeds an accumulator and is not. */
static int nested(int n)
{
    int t = 0;
    for (int o = 0; o < n; o++) {
        int r = 0;
        for (int i = 0; i < n; i++)
            r = 9;
        t += r;
    }
    return t;
}

/* A volatile read is not a side effect in ssa_opt_has_side_effects, so the
 * loop has to be held by the function-level volatile bail instead. */
static int volatile_poll(int n)
{
    int r = 0;
    for (int i = 0; i < n; i++)
        r = vsink;
    return r;
}

int main(void)
{
    int bounds[4] = {0, 1, 5, 9};

    vsink = 42;
    for (int k = 0; k < 4; k++) {
        int n = bounds[k];
        printf("%d: %d %d %d %d %d %u %d %u %d %d %d %d %d %d %d %d %d\n", n,
               const_result(n), mangle(n), counter_escapes(n), store_each(n),
               down_counting(n), unsigned_bound((unsigned)n), step_two(n),
               near_max((unsigned)n), with_break(n), with_continue(n),
               shrinking_bound(n), load_only(n), load_varying(n), do_while(n),
               no_iv(n), nested(n), volatile_poll(n));
    }
    printf("%u %u\n", near_max(0xfffffff2u), near_max(0xffffffffu));
    return 0;
}
