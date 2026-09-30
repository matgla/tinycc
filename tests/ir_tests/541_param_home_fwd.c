/* A by-value struct parameter is homed in the frame by entry-block word
   stores, and field reads that lea folding turned into direct slot loads
   reloaded it on every loop turn: Zig's StaticStringMap.defaultEql compared
   two slices with both kept in memory.  In a leaf function the reads now take
   the parameter's register.  Beside that shape, the ones that must keep the
   home: a field written after entry, a read through a pointer to the
   parameter, and a narrow read of a word. */
#include <stdio.h>
#include <stdint.h>

struct slice { const uint8_t *ptr; uintptr_t len; };

__attribute__((noinline)) static int eql(struct slice a, struct slice b)
{
    if (a.ptr == b.ptr)
        return 1;
    for (uintptr_t i = 0; i < a.len; i++)
        if (a.ptr[i] != b.ptr[i])
            return 0;
    return 1;
}

/* a.len is rewritten: the home is no longer the entry value */
__attribute__((noinline)) static uintptr_t count_upto(struct slice a, uintptr_t cap)
{
    if (a.len > cap)
        a.len = cap;
    uintptr_t n = 0;
    for (uintptr_t i = 0; i < a.len; i++)
        n += a.ptr[i] & 1;
    return n;
}

/* read through a pointer to the parameter */
__attribute__((noinline)) static uintptr_t via_ptr(struct slice a, int k)
{
    struct slice *p = &a;
    if (k)
        p->len = p->len / 2;
    uintptr_t s = 0;
    for (uintptr_t i = 0; i < p->len; i++)
        s += p->ptr[i];
    return s + a.len;
}

/* a narrow read of the word */
__attribute__((noinline)) static unsigned low_byte_sum(struct slice a)
{
    unsigned s = 0;
    for (uintptr_t i = 0; i < a.len; i++)
        s += (uint8_t)a.len + a.ptr[i];
    return s;
}

int main(void)
{
    static const uint8_t x[] = "hello, world";
    static const uint8_t y[] = "hello, there";
    struct slice a = {x, 12}, b = {y, 12}, c = {x, 5}, d = {y, 5};
    printf("eql %d %d %d %d\n", eql(a, b), eql(c, d), eql(a, a), eql((struct slice){x + 1, 3}, (struct slice){y + 1, 3}));
    printf("count %u %u\n", (unsigned)count_upto(a, 4), (unsigned)count_upto(a, 100));
    printf("ptr %u %u\n", (unsigned)via_ptr(a, 0), (unsigned)via_ptr(a, 1));
    printf("low %u\n", low_byte_sum((struct slice){x, 260 - 250}));
    return 0;
}
