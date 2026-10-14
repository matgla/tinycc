/* Copies between two frame objects made of word loads and stores (a struct
   assignment, an inlined by-value struct parameter) share the objects' bytes
   when the source dies in the run and the destination is born there
   (frame.c frame_find_word_copies): the run is deleted.  Positive shapes --
   an inlined parameter copy, a chain of them, a join object written on two
   paths -- and the near misses that must keep both objects: a source read or
   rewritten while the copy is live, a copy in a loop that keeps the previous
   iteration's value. */
#include <stdio.h>
#include <stdint.h>

typedef struct { uint32_t w[6]; } S;
typedef struct { uint32_t a, b; uint16_t err; } R;

static uint32_t hash(const S *p)
{
    uint32_t h = 5;
    for (int i = 0; i < 6; i++)
        h = h * 31 + p->w[i];
    return h;
}
__attribute__((noinline)) static uint32_t use(const S *p) { return hash(p); }
__attribute__((noinline)) static S make(uint32_t k)
{
    S s;
    for (int i = 0; i < 6; i++)
        s.w[i] = k * 7 + i;
    return s;
}
__attribute__((noinline)) static void bump(S *p) { p->w[2] += 100; }
__attribute__((noinline)) static R get(uint32_t k)
{
    R r = { k * 3, k + 7, (uint16_t)(k & 1) };
    return r;
}

static uint32_t callee(S v, uint32_t k)
{
    v.w[0] += k;
    return use(&v);
}

/* An inlined by-value parameter: the copy of a temporary that dies. */
__attribute__((noinline)) static uint32_t f1(uint32_t k)
{
    S a = make(k);
    return callee(a, k);
}

/* A join object written on two paths. */
__attribute__((noinline)) static uint32_t f2(uint32_t k)
{
    S r;
    if (k & 1)
    {
        S t = make(k);
        r = t;
    }
    else
    {
        S t = make(k + 3);
        bump(&t);
        r = t;
    }
    return use(&r);
}

/* The source is read after the copy and the destination modified. */
__attribute__((noinline)) static uint32_t f3(uint32_t k)
{
    S a = make(k);
    S b = a;
    b.w[1] = 99;
    bump(&b);
    return use(&a) ^ use(&b);
}

/* The source is rewritten while the copy is live. */
__attribute__((noinline)) static uint32_t f4(uint32_t k)
{
    S a = make(k);
    S b = a;
    a = make(k + 1);
    return use(&b) * 3 + use(&a);
}

/* A chain of copies through inlined callees. */
static uint32_t inner(S v, uint32_t k) { return use(&v) + k; }
static uint32_t outer(S v, uint32_t k)
{
    v.w[5] ^= k;
    return inner(v, k) + inner(v, k + 1);
}
__attribute__((noinline)) static uint32_t f5(uint32_t k)
{
    S a = make(k);
    return outer(a, k);
}

/* Copies in a loop, one of them keeping the previous iteration's value. */
__attribute__((noinline)) static uint32_t f6(uint32_t n)
{
    uint32_t h = 0;
    S keep = make(1);
    for (uint32_t i = 0; i < n; i++)
    {
        S t = make(i);
        S c = t;
        if (i & 1)
            c = keep;
        keep = c;
        h += use(&c);
    }
    return h + use(&keep);
}

/* A join object read field by field (never escapes): liveness, not the
   lifetimes' hulls, proves it apart from the call result it copies. */
__attribute__((noinline)) static uint32_t g1(uint32_t k)
{
    R d;
    if (k > 5)
    {
        d.a = 0xaaaaaaaa;
        d.b = 0xaaaaaaaa;
        d.err = 9;
    }
    else
    {
        R s = get(k);
        d = s;
    }
    if (d.err)
        return d.err;
    return d.a + d.b;
}

/* The same, with the source still read on one path after the copy. */
__attribute__((noinline)) static uint32_t g2(uint32_t k)
{
    R d;
    R s = get(k);
    if (k > 5)
    {
        d.a = 1;
        d.b = 2;
        d.err = 0;
    }
    else
        d = s;
    if (k & 2)
        return s.a * 5 + d.b;
    return d.a + d.b + d.err;
}

/* The source rewritten on a later path while the copy is still read. */
__attribute__((noinline)) static uint32_t g3(uint32_t n)
{
    uint32_t h = 0;
    R d = get(100);
    for (uint32_t i = 0; i < n; i++)
    {
        R s = get(i);
        if (i & 1)
            d = s;
        h = h * 3 + d.a + d.b + d.err + s.a;
    }
    return h;
}

/* One word moved in place, the other two crossed around a call: merging
   would let the first crossed store clobber the source word the second still
   reads. */
static volatile uint32_t ticks;
__attribute__((noinline)) static void tick(void) { ticks++; }
__attribute__((noinline)) static uint32_t g4(uint32_t k)
{
    R s = get(k);
    R d;
    d.err = s.err;
    d.a = s.b;
    tick();
    d.b = s.a;
    if (d.err)
        return d.a * 3 + d.b;
    return d.a * 5 + d.b;
}

int main(void)
{
    for (uint32_t k = 0; k < 4; k++)
        printf("%u %u %u %u %u\n", (unsigned)f1(k), (unsigned)f2(k), (unsigned)f3(k), (unsigned)f4(k),
               (unsigned)f5(k));
    printf("%u\n", (unsigned)f6(7));
    for (uint32_t k = 0; k < 8; k++)
        printf("%u %u ", (unsigned)g1(k), (unsigned)g2(k));
    printf("\n%u\n", (unsigned)g3(9));
    for (uint32_t k = 0; k < 4; k++)
        printf("%u ", (unsigned)g4(k));
    printf("\n");
    return 0;
}
