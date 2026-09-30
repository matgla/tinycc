/* A small struct passed BY VALUE from a local whose fields SRA replaced is
   passed as one argument per word, sourced from the field VARs.  For a
   4-aligned, float-free 8/12/16-byte struct AAPCS places the words exactly
   where N word arguments go: from the NCRN, straddling r3 onto the stack.
   Shapes: the Zig slice (2 words, r0-r1), two split structs in one call with
   a scalar after them (later arguments renumbered), 3 words straddling r3,
   4 words wholly on the stack, a narrow field at a word's start, a word no
   field covers, and an 8-aligned struct that keeps its memory (it would start
   at an even register). */
#include <stdio.h>
#include <stdint.h>

struct slice { uint8_t const *ptr; uint32_t len; };
struct alloc { void *ctx; uint32_t id; };
struct three { uint32_t a, b, c; };
struct four { uint32_t a, b, c, d; };
struct eu { uint32_t payload; uint16_t error; };
struct gap { uint32_t a, unused, c; };
struct wide { uint64_t v; };

__attribute__((noinline)) static uint32_t sum(struct slice s)
{
    uint32_t r = 0;
    for (uint32_t i = 0; i < s.len; i++)
        r = r * 31 + s.ptr[i];
    return r;
}

__attribute__((noinline)) static uint32_t take(struct alloc a, struct slice s, uint32_t n)
{
    return a.id * 1000 + s.len * 10 + n + (s.ptr[0] == 'e');
}

__attribute__((noinline)) static uint32_t straddle(uint32_t x, uint32_t y, uint32_t z, struct three t)
{
    return x + y * 10 + z * 100 + t.a * 1000 + t.b * 10000 + t.c * 100000;
}

__attribute__((noinline)) static uint32_t on_stack(uint32_t w, uint32_t x, uint32_t y, uint32_t z, struct four f)
{
    return w + x + y + z + f.a * 10 + f.b * 100 + f.c * 1000 + f.d * 10000;
}

__attribute__((noinline)) static uint32_t check_eu(struct eu e)
{
    return e.error ? 100000u + e.error : e.payload;
}

__attribute__((noinline)) static uint32_t check_gap(struct gap g)
{
    return g.a * 7 + g.c;
}

__attribute__((noinline)) static uint64_t check_wide(uint32_t x, struct wide w)
{
    return w.v + x;
}

__attribute__((noinline)) static uint32_t caller(uint8_t const *p, uint32_t n, void *ctx, uint32_t k)
{
    struct slice t0;
    struct alloc t1;
    struct three t2;
    struct four t3;
    struct eu t4;
    struct gap t5;
    struct wide t6;
    t0.ptr = p + 1;
    t0.len = n - 1;
    t1.ctx = ctx;
    t1.id = k;
    uint32_t r = sum(t0);
    r += take(t1, t0, k + 3);
    t2.a = 4; t2.b = 5; t2.c = k;
    r += straddle(1, 2, 3, t2);
    t3.a = 1; t3.b = 2; t3.c = 3; t3.d = k;
    r += on_stack(9, 8, 7, 6, t3);
    t4.payload = n * 3;
    t4.error = (uint16_t)(k > 5 ? k : 0);
    r += check_eu(t4);
    t5.a = k;
    t5.c = n;
    r += check_gap(t5);
    t6.v = ((uint64_t)k << 32) | n;
    r += (uint32_t)(check_wide(2, t6) >> 32);
    return r;
}

int main(void)
{
    static const uint8_t text[] = "hello";
    int dummy;
    printf("%u\n", (unsigned)caller(text, 5, &dummy, 2));
    printf("%u\n", (unsigned)caller(text, 3, &dummy, 9));
    return 0;
}
