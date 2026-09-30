/* Scalar replacement of struct locals a call accesses whole: an aggregate
   over 16 bytes copied with __aeabi_memmove4/8 into or out of the object, and
   a struct-returning call writing its result buffer.  The copy becomes one
   load or store per field and goes; the buffer keeps its slot and the fields
   are read back after the call.  Bytes the object only passes on (copied in,
   then copied out or passed by value) need fields too, or they are lost. */
#include <stdio.h>
#include <stdint.h>

struct six { uint32_t a, b, c, d, e, f; };
struct wide { uint64_t a; uint32_t b, c; uint64_t d; };
struct narrow { uint8_t a; uint8_t pad; uint16_t b; uint32_t c, d, e, f, g; };
struct outer { uint32_t tag; struct six in; };

__attribute__((noinline)) static struct six make(uint32_t x)
{
    struct six r = {x, x + 1, x + 2, x + 3, x + 4, x + 5};
    return r;
}

/* Copied in from a pointer, every field read. */
__attribute__((noinline)) static uint32_t copy_in(struct six const *p, int n)
{
    struct six t0;
    uint32_t acc = 0, a, b, c, d, e, f;
    t0 = *p;
    a = t0.a;
    b = t0.b;
    c = t0.c;
    d = t0.d;
    e = t0.e;
    f = t0.f;
    for (int i = 0; i < n; i++)
        acc = acc * 3 + a + b * 2 + c * 3 + d * 5 + e * 7 + f * 11;
    return acc;
}

/* Built field by field, copied out. */
__attribute__((noinline)) static void copy_out(struct six *out, uint32_t v)
{
    struct six t0;
    t0.a = v;
    t0.b = v * 2;
    t0.c = v * 3;
    t0.d = v * 4;
    t0.e = v * 5;
    t0.f = v * 6;
    *out = t0;
}

/* Copied in, one field changed, copied out: the others pass through. */
__attribute__((noinline)) static void through(struct six const *p, struct six *out, uint32_t v)
{
    struct six t0;
    uint32_t x;
    t0 = *p;
    x = t0.c;
    t0.c = x + v;
    *out = t0;
}

/* A result buffer, its fields read. */
__attribute__((noinline)) static uint32_t result(uint32_t x)
{
    struct six t0;
    uint32_t a, f;
    t0 = make(x);
    a = t0.a;
    f = t0.f;
    return a * 100 + f;
}

/* A result buffer copied out after a field changed: the bytes nothing reads
   one at a time pass through too. */
__attribute__((noinline)) static void result_through(struct six *out, uint32_t x)
{
    struct six t0;
    uint32_t a, b, c, d;
    t0 = make(x);
    a = t0.a;
    b = t0.b;
    c = t0.c;
    d = t0.d;
    t0.b = a + b * 10 + c + d;
    t0.d = d * 2;
    *out = t0;
}

/* A result written into a member of a bigger object. */
__attribute__((noinline)) static uint32_t result_member(uint32_t x)
{
    struct outer o;
    uint32_t t, a, e;
    o.tag = 7;
    o.in = make(x);
    t = o.tag;
    a = o.in.a;
    e = o.in.e;
    return t * 1000 + a + e;
}

/* 64-bit fields through a copy. */
__attribute__((noinline)) static void wide_through(struct wide const *p, struct wide *out)
{
    struct wide t0;
    uint64_t a;
    t0 = *p;
    a = t0.a;
    t0.a = a + 0x100000001ull;
    *out = t0;
}

/* Narrow fields through a copy. */
__attribute__((noinline)) static uint32_t narrow_through(struct narrow const *p, struct narrow *out)
{
    struct narrow t0;
    uint32_t s;
    t0 = *p;
    s = t0.a + t0.b;
    t0.a = (uint8_t)(t0.a + 1);
    *out = t0;
    return s;
}

/* A copy between two objects both could replace. */
__attribute__((noinline)) static uint32_t pair(struct six const *p, int k)
{
    struct six t0, t1;
    struct six *q = &t1;
    uint32_t a, f;
    t0 = *p;
    a = t0.a;
    t0.a = a + (uint32_t)k;
    *q = t0;
    a = t1.a;
    f = t1.f;
    return a + f;
}

static void show(const char *n, struct six const *s)
{
    printf("%s %u %u %u %u %u %u\n", n, s->a, s->b, s->c, s->d, s->e, s->f);
}

int main(void)
{
    struct six s = {1, 2, 3, 4, 5, 6}, o;
    printf("copy_in %u %u\n", copy_in(&s, 0), copy_in(&s, 3));
    copy_out(&o, 9);
    show("copy_out", &o);
    through(&s, &o, 40);
    show("through", &o);
    printf("result %u\n", result(20));
    result_through(&o, 30);
    show("result_through", &o);
    printf("result_member %u\n", result_member(50));
    struct wide w = {0x1111111122222222ull, 3, 4, 0x3333333344444444ull}, wo;
    wide_through(&w, &wo);
    printf("wide %llx %u %u %llx\n", (unsigned long long)wo.a, wo.b, wo.c, (unsigned long long)wo.d);
    struct narrow nr = {200, 0, 60000, 1, 2, 3, 4, 5}, no;
    uint32_t ns = narrow_through(&nr, &no);
    printf("narrow %u %u %u %u %u %u %u %u\n", ns, no.a, no.b, no.c, no.d, no.e, no.f, no.g);
    printf("pair %u\n", pair(&s, 100));
    return 0;
}
