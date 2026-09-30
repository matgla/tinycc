/* Constant-size copies of 32 bytes and up are expanded inline instead of
   calling memcpy: a by-value struct argument of eight words or more placed on
   the stack (unrolled LDM/STM to 16 words, a counted loop above), a BLOCK_COPY
   of 64 bytes or more from .rodata (a strcpy of a long literal, byte tail
   included), an aligned struct assignment over 128 bytes, a local array
   initialised from a .rodata template, and a string literal filling a char
   array field of a word-aligned local (Zig's `undefined` fill) -- next to one
   at an odd offset, which keeps its unaligned memcpy.  Every stack-struct
   size from 8 to 26 words -- each remainder of both the unrolled and the
   looped form -- plus one of 75 words, with register arguments live around
   the copy. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define S(n) struct s##n { uint32_t w[n]; };
S(8) S(9) S(10) S(11) S(12) S(13) S(15) S(16) S(17) S(18) S(19) S(20) S(26) S(75)

static uint32_t mix(const uint32_t *w, int n, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 3 + b * 5 + c * 7;
    for (int i = 0; i < n; i++)
        h = h * 31 + w[i];
    return h;
}

/* Three register args first, so the struct is split: r3 plus the stack. */
#define F(n) __attribute__((noinline)) static uint32_t f##n(uint32_t a, uint32_t b, uint32_t c, struct s##n s) \
    { return mix(s.w, n, a, b, c); }
F(8) F(9) F(10) F(11) F(12) F(13) F(15) F(16) F(17) F(18) F(19) F(20) F(26) F(75)

/* Four register args first: the struct is all on the stack. */
__attribute__((noinline)) static uint32_t g17(uint32_t a, uint32_t b, uint32_t c, uint32_t d, struct s17 s)
{
    return mix(s.w, 17, a, b, c + d);
}

/* A struct with a byte tail passed by value. */
struct odd { uint32_t w[9]; uint8_t t[3]; };
__attribute__((noinline)) static uint32_t fodd(uint32_t a, struct odd o)
{
    return mix(o.w, 9, a, o.t[0] | o.t[1] << 8, o.t[2]);
}

/* Leaf functions: the copy clobbers LR, which they did not save. */
__attribute__((noinline)) static uint32_t leaf_strcpy(int k)
{
    char buf[80];
    strcpy(buf, "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-+*/!?");
    uint32_t h = 0;
    for (int i = 0; buf[i]; i++)
        h = h * 33 + (unsigned char)buf[i] + (uint32_t)k;
    return h + (uint32_t)strlen(buf);
}

__attribute__((noinline)) static uint32_t leaf_table(int i)
{
    const uint32_t t[24] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4, 6, 2, 6, 4};
    return t[i % 24] * 100 + t[(i + 7) % 24];
}

struct big { uint32_t w[50]; };
__attribute__((noinline)) static void assign(struct big *d, const struct big *s) { *d = *s; }

#define AA "\252\252\252\252\252\252\252\252\252\252\252\252"
struct wy { uint64_t seed; uint32_t a, b; char buf[48]; };
__attribute__((noinline)) static uint32_t wy_fill(uint32_t k)
{
    struct wy w = {k, k * 3, k ^ 5, {AA AA AA AA}};
    w.buf[k % 48] = 1;
    return mix((const uint32_t *)w.buf, 12, (uint32_t)w.seed, w.a, w.b);
}

struct odd_off { char c; char buf[41]; };
__attribute__((noinline)) static uint32_t odd_fill(uint32_t k)
{
    struct odd_off o = {(char)k, {"0123456789abcdefghijklmnopqrstuvwxyzABCD"}};
    uint32_t h = (unsigned char)o.c;
    for (int i = 0; i < 41; i++)
        h = h * 31 + (unsigned char)o.buf[i];
    return h;
}

int main(void)
{
    uint32_t acc = 0, seed = 0x12345678;
#define R() (seed = seed * 1103515245u + 12345u)
#define CALL(n) { struct s##n s; for (int i = 0; i < n; i++) s.w[i] = R(); \
        uint32_t a = R(), b = R(), c = R(); acc = acc * 7 + f##n(a, b, c, s); \
        acc ^= mix(s.w, n, a, b, c) == f##n(a, b, c, s) ? 0 : 0xdead0000u + n; }
    CALL(8) CALL(9) CALL(10) CALL(11) CALL(12) CALL(13) CALL(15) CALL(16)
    CALL(17) CALL(18) CALL(19) CALL(20) CALL(26) CALL(75)
    printf("byval %08x\n", acc);

    struct s17 s;
    for (int i = 0; i < 17; i++) s.w[i] = R();
    printf("stack-only %d\n", g17(1, 2, 3, 4, s) == mix(s.w, 17, 1, 2, 7));

    struct odd o;
    for (int i = 0; i < 9; i++) o.w[i] = R();
    o.t[0] = 0x11; o.t[1] = 0x22; o.t[2] = 0x33;
    printf("odd %d\n", fodd(5, o) == mix(o.w, 9, 5, 0x2211, 0x33));

    printf("strcpy %08x\n", leaf_strcpy(3));
    printf("table %u %u %u\n", leaf_table(0), leaf_table(11), leaf_table(23));

    static struct big a, b;
    for (int i = 0; i < 50; i++) { a.w[i] = R(); b.w[i] = 0; }
    assign(&b, &a);
    assign(&a, &a);
    printf("assign %d\n", !memcmp(&a, &b, sizeof a));
    printf("wy %08x %08x\n", wy_fill(7), wy_fill(40));
    printf("oddoff %08x\n", odd_fill(9));
    return 0;
}
