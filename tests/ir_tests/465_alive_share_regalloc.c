/* ra:alive_share — sharing a register whose owner is provably dead.
 *
 * The linear scan models every value as ONE [start,end] range, which is a
 * strict over-approximation across a diamond: a value read only in the `then`
 * arm looks live in the `else` arm too, so a temp defined there sees no free
 * register and goes to the frame.  `__aeabi_dadd`'s alignment block is the
 * shape -- a 64-bit temp was stored and reloaded at the very next instruction
 * while `exp_diff`'s register sat idle on that path.
 *
 * Accurate liveness says the two never overlap, so they can share.  What has
 * to be proved is that nothing reads a register after another value has been
 * written over it, which is a wrong-VALUE bug, not a crash -- so every case
 * below returns data that changes if a share clobbers something, and the whole
 * thing runs at every -O level against the same expected output.
 *
 * Cases: per-arm liveness with enough simultaneous 64-bit values to force the
 * spill path; a value live only AFTER the diamond; a call inside the range (a
 * shared register must then be callee-saved); an address-taken local (never a
 * donor); a value read on the path that does NOT define the sharer; and a
 * single-use return feeder, which is the one other mechanism that puts a
 * second owner on a register.
 */
#include <stdio.h>

volatile int vzero = 0;
volatile unsigned long long vsink;

/* dadd's shape: two 64-bit values live across a diamond, a third defined and
 * consumed inside one arm, and `sel` read in BOTH arms so its interval spans
 * the whole region while its accurate liveness does not. */
static unsigned long long arm_local(unsigned long long a, unsigned long long b, int sel)
{
    unsigned long long r;
    if (sel > 0)
    {
        unsigned long long lost = b << (64 - sel);
        r = (b >> sel) | (lost != 0);
        r += a ^ (unsigned long long)sel;
    }
    else if (sel < 0)
    {
        unsigned long long lost = a << (64 + sel);
        r = (a >> -sel) | (lost != 0);
        r += b ^ (unsigned long long)(-sel);
    }
    else
        r = a + b;
    return r * 3 + (a ^ b);
}

/* The donor is live only AFTER the diamond, so its interval starts before the
 * sharer's and ends after it -- the case the [start,end] test must reject. */
static unsigned long long live_after(unsigned long long a, unsigned long long b, int sel)
{
    unsigned long long keep = a * 0x9E3779B97F4A7C15ULL + b;
    unsigned long long t;
    if (sel)
        t = (a << 13) ^ (b >> 7) ^ (a + b);
    else
        t = (b << 11) ^ (a >> 5) ^ (a * b);
    return t ^ keep ^ (keep >> 32);
}

static unsigned long long sink64(unsigned long long v)
{
    vsink = v;
    return v ^ 0x5555555555555555ULL;
}

/* A call inside the range: anything sharing a register across it has to be in
 * a callee-saved one, or the callee's clobber eats the value. */
static unsigned long long across_call(unsigned long long a, unsigned long long b, int sel)
{
    unsigned long long x = a ^ (b << 3);
    unsigned long long y;
    if (sel > 0)
        y = sink64(x + 1) ^ (a >> 9);
    else
        y = sink64(x + 2) ^ (b >> 11);
    return y + x + a + b;
}

/* An address-taken local must never be a donor: its home is the frame and the
 * pointer can be read through at any point the scan cannot see. */
static unsigned long long addr_taken(unsigned long long a, unsigned long long b, int sel)
{
    unsigned long long slot = a ^ b;
    unsigned long long *p = &slot;
    unsigned long long t;
    if (sel > 0)
        t = (a >> 3) | ((a << 61) != 0);
    else
        t = (b >> 5) | ((b << 59) != 0);
    *p += t;
    return slot * 7 + t;
}

/* Single-use RETURNVALUE feeder — the return-block share, interlocked with
 * this one because it leaves its second owner out of the active set. */
static unsigned long long ret_feeder(unsigned long long a, unsigned long long b, int sel)
{
    unsigned long long u = a + b;
    unsigned long long v = a - b;
    unsigned long long w = a ^ b;
    unsigned long long z = (a << 1) | (b >> 63);
    if (sel > 0)
        return u ^ v ^ w ^ z ^ (a >> 17);
    return u + v + w + z + (b >> 19);
}

/* Deferred call arguments: the PARAM sources are read again at the CALL, so a
 * value marshalled early must not be a donor for anything defined between. */
static unsigned long long deferred_params(unsigned long long a, unsigned long long b, int sel)
{
    unsigned long long p0 = a ^ 0x0123456789ABCDEFULL;
    unsigned long long p1 = b + 0x0FEDCBA987654321ULL;
    unsigned long long mid = sel > 0 ? (a << 7) : (b >> 7);
    unsigned long long s = sink64(p0) + sink64(p1);
    return s ^ mid;
}

int main(void)
{
    static const unsigned long long vals[] = {
        0ULL, 1ULL, 0xFFFFFFFFULL, 0x100000000ULL,
        0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL, ~0ULL, 0x8000000000000000ULL};
    static const int sels[] = {-63, -32, -7, -1, 0, 1, 7, 32, 63};
    unsigned long long acc = 0;
    int i, j, k;

    for (i = 0; i < (int)(sizeof vals / sizeof vals[0]); i++)
        for (j = 0; j < (int)(sizeof vals / sizeof vals[0]); j++)
            for (k = 0; k < (int)(sizeof sels / sizeof sels[0]); k++)
            {
                unsigned long long a = vals[i] + (unsigned)vzero;
                unsigned long long b = vals[j] + (unsigned)vzero;
                int s = sels[k] + vzero;
                acc = acc * 31 + arm_local(a, b, s);
                acc = acc * 31 + live_after(a, b, s);
                acc = acc * 31 + across_call(a, b, s);
                acc = acc * 31 + addr_taken(a, b, s);
                acc = acc * 31 + ret_feeder(a, b, s);
                acc = acc * 31 + deferred_params(a, b, s);
            }

    printf("acc %016llx\n", (unsigned long long)acc);
    printf("sink %016llx\n", (unsigned long long)vsink);
    return 0;
}
