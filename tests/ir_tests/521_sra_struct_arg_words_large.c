/* Larger by-value structs from SRA-replaced locals, passed one argument per
   word (up to SRA's 32 bytes).  Shapes: Zig's std.fmt.Options (24 bytes,
   optional usize = word + bool, the bools written through an indexed narrow
   store) after a pointer, straddling r1-r3 onto the stack, with a scalar after
   it; the same object passed to two calls; 28 bytes wholly on the stack with
   words no field covers; exactly 32 bytes; 36 bytes (over SRA's limit: keeps
   its memory); and two narrow fields in one word (keeps its memory: the word
   is no single field). */
#include <stdio.h>
#include <stdint.h>

struct opt_usize { uint32_t v; uint8_t is_null; };
struct options { struct opt_usize precision, width; uint8_t alignment; uint32_t fill; };
struct seven { uint32_t w[7]; };
struct eight { uint32_t w[8]; };
struct nine { uint32_t w[9]; };
struct packed2 { uint8_t lo, hi; uint32_t rest[5]; };

__attribute__((noinline)) static uint32_t print_value(void *w, struct options o, uint32_t depth)
{
    uint32_t r = w != 0;
    r += o.precision.is_null ? 7 : o.precision.v * 3;
    r += o.width.is_null ? 11 : o.width.v * 5;
    return r * 1000 + o.alignment * 100 + o.fill + depth * 13;
}

__attribute__((noinline)) static uint32_t align_buffer(struct options o)
{
    return (o.width.is_null ? 0 : o.width.v) + o.fill;
}

__attribute__((noinline)) static uint32_t sum7(uint32_t a, uint32_t b, uint32_t c, uint32_t d, struct seven f)
{
    uint32_t r = a + b * 3 + c * 5 + d * 7;
    for (int i = 0; i < 7; i++)
        if (i != 3)
            r = r * 17 + f.w[i];
    return r;
}

__attribute__((noinline)) static uint32_t sum8(uint32_t a, struct eight f, uint32_t z)
{
    uint32_t r = a;
    for (int i = 0; i < 8; i++)
        r = r * 31 + f.w[i];
    return r ^ z;
}

__attribute__((noinline)) static uint32_t sum9(struct nine f)
{
    uint32_t r = 0;
    for (int i = 0; i < 9; i++)
        r = r * 29 + f.w[i];
    return r;
}

__attribute__((noinline)) static uint32_t sum_packed(uint32_t a, struct packed2 p)
{
    return a + p.lo * 3 + p.hi * 5 + p.rest[0] + p.rest[4] * 9;
}

__attribute__((noinline)) static uint32_t caller(void *w, uint32_t n, uint32_t k)
{
    struct options o;
    struct seven f;
    struct eight s;
    struct nine e;
    struct packed2 p;
    o.precision.v = 0;
    o.precision.is_null = 1;
    o.width.v = n;
    o.width.is_null = k > 4;
    o.alignment = 2;
    o.fill = ' ';
    uint32_t r = print_value(w, o, k + 1);
    o.fill = '0' + k;
    r += align_buffer(o);
    r += print_value(0, o, 0);
    f.w[0] = k; f.w[1] = n + k; f.w[2] = n * 2 + k;
    f.w[4] = n * 4 + k; f.w[5] = n * 5 + k; f.w[6] = n * 6 + k;
    r += sum7(1, 2, 3, 4, f);
    s.w[0] = k; s.w[1] = 1 ^ k; s.w[2] = 2 ^ k; s.w[3] = 3 ^ k;
    s.w[4] = 4 ^ k; s.w[5] = 5 ^ k; s.w[6] = 6 ^ k; s.w[7] = 7 ^ k;
    r += sum8(n, s, 0x55);
    e.w[0] = n; e.w[1] = 1 + n; e.w[2] = 2 + n; e.w[3] = 3 + n; e.w[4] = 4 + n;
    e.w[5] = 5 + n; e.w[6] = 6 + n; e.w[7] = 7 + n; e.w[8] = 8 + n;
    r += sum9(e);
    p.lo = (uint8_t)n;
    p.hi = (uint8_t)k;
    p.rest[0] = n + k;
    p.rest[4] = n * k;
    r += sum_packed(3, p);
    return r;
}

int main(void)
{
    int dummy;
    printf("%u\n", (unsigned)caller(&dummy, 5, 2));
    printf("%u\n", (unsigned)caller(&dummy, 9, 7));
    return 0;
}
