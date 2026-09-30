/* Scalar replacement of struct locals reached through pointer VARs -- the
   Zig C backend's `t1 = &t0; t5 = &t1->len; x = *t5;`.  A pointer local is
   followed within a block (and everywhere when its only definition is one
   `&obj`); its fields become VARs.  Shapes: a pointer reused for different
   fields, a pointer copied to another, one defined before a loop and read
   in it, writes through it read back directly, and the ones that must keep
   memory -- a pointer set on two paths and read after the join, compared,
   passed to a call, stored, or indexed at run time. */
#include <stdio.h>
#include <stdint.h>

struct mal { uint8_t *bytes; uint32_t len, cap; };
struct pair { uint32_t a, b; };

__attribute__((noinline)) static uint32_t reuse(struct mal const a0)
{
    struct mal t0;
    struct mal const *t1;
    uint32_t const *t5;
    uint32_t x, y;
    t0 = a0;
    t1 = &t0;
    t5 = &t1->len;
    x = *t5;
    t5 = &t1->cap;
    y = *t5;
    return x * 1000 + y + (uint32_t)(t1->bytes != 0);
}

__attribute__((noinline)) static uint32_t copy_and_loop(struct pair const a0, int n)
{
    struct pair t0 = a0;
    struct pair *t1 = &t0, *t2;
    uint32_t acc = 0;
    t2 = t1;
    for (int i = 0; i < n; i++) {
        t2->a += (uint32_t)i;
        acc += t1->b * t2->a;
    }
    return acc + t0.a;
}

__attribute__((noinline)) static uint32_t write_through(uint32_t v)
{
    struct pair t0 = {1, 2};
    uint32_t *p = &t0.b;
    *p = v;
    p = &t0.a;
    *p += 5;
    return t0.a * 100 + t0.b;
}

__attribute__((noinline)) static uint32_t join(int c, uint32_t v)
{
    struct pair x = {10, 20}, y = {30, 40};
    uint32_t *p;
    if (c)
        p = &x.a;
    else
        p = &y.b;
    *p = v;
    return x.a + x.b * 3 + y.a * 5 + y.b * 7;
}

__attribute__((noinline)) static int is_first(const struct pair *p, const struct pair *q) { return p == q; }

__attribute__((noinline)) static uint32_t escapes(uint32_t v)
{
    struct pair t0 = {v, v + 1};
    struct pair *t1 = &t0;
    uint32_t r = t1->a;
    if (t1 != 0 && is_first(t1, &t0))
        r += 100;
    static struct pair *keep;
    keep = t1;
    keep->b += 7;
    return r + t0.b;
}

__attribute__((noinline)) static uint32_t indexed(uint32_t i)
{
    struct pair t0 = {3, 4};
    struct pair *t1 = &t0;
    uint32_t *w = &t1->a;
    return w[i & 1] * 10 + t0.a;
}

/* The address reaches a pointer through a TEMP (`T <- t1; t5 <- T + 4`) and
   is used in another block: t5 must count as a pointer that may hold it. */
__attribute__((noinline)) static uint32_t other_block(struct pair const a0, int c, uint32_t v)
{
    struct pair t0;
    struct pair *t1;
    uint32_t *t5;
    t0 = a0;
    t1 = &t0;
    t5 = &t1->b;
    if (c)
        *t5 = v;
    return t0.b + *t5;
}

/* One local reused for an unrelated pointer (Zig reuses locals by type): its
   loaded value is no frame address, and t0 stays promotable. */
__attribute__((noinline)) static uint32_t reused_local(struct pair const a0, uint32_t *const *pp)
{
    struct pair t0;
    uint32_t *t5;
    uint32_t x;
    t0 = a0;
    t5 = &t0.a;
    x = *t5;
    t5 = *pp;
    x += *t5;
    return x + t0.b;
}

/* Across blocks by dataflow: the same address on both paths into a join
   (promotable), different ones (kept), and a pointer moved inside a loop. */
__attribute__((noinline)) static uint32_t df_agree(struct pair const a0, int c)
{
    struct pair t0 = a0;
    uint32_t *t5 = &t0.b, acc = 1;
    if (c)
        acc += 5;
    else
        acc *= 3;
    return acc * 100 + *t5;
}

__attribute__((noinline)) static uint32_t df_loop(struct pair const a0, int n)
{
    struct pair t0 = a0;
    uint32_t *t5 = &t0.a, acc = 0;
    for (int i = 0; i < n; i++) {
        acc = acc * 7 + *t5;
        if (i & 1)
            t5 = &t0.b;
        *t5 += 1;
    }
    return acc + t0.a * 11 + t0.b;
}

int main(void)
{
    static uint8_t mem[4];
    struct mal m = {mem, 7, 9};
    printf("reuse %u\n", reuse(m));
    struct pair pr = {3, 5};
    printf("loop %u %u\n", copy_and_loop(pr, 0), copy_and_loop(pr, 6));
    printf("write %u\n", write_through(42));
    printf("join %u %u\n", join(1, 99), join(0, 99));
    printf("escapes %u\n", escapes(11));
    printf("indexed %u %u\n", indexed(0), indexed(1));
    printf("other_block %u %u\n", other_block(pr, 1, 77), other_block(pr, 0, 77));
    uint32_t cell = 1000, *cp = &cell;
    printf("reused %u\n", reused_local(pr, &cp));
    printf("df %u %u %u %u\n", df_agree(pr, 1), df_agree(pr, 0), df_loop(pr, 0), df_loop(pr, 7));
    return 0;
}
