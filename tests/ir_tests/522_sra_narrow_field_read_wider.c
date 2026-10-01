/* A field stored at one width and loaded at others.
   - Loaded wider: a narrow field stored only at its own width, then read as a
     halfword or a word whose other bytes nothing writes.  SRA makes it a field
     VAR and the wider loads read that VAR (the bytes beyond it hold nothing,
     so its zero bits stand for them).  Shapes: Zig's error union
     `{ uint16_t error; }` returned by value (loaded as a word); a 4-aligned
     one-byte struct returned the same way; an optional's flag byte read as a
     u16 and at its own width.  The callers mask off the never-written bytes.
   - Loaded wider across several narrow fields: two bytes stored one at a time
     and read back as one halfword (their VARs shifted and ORed), unsigned and
     signed (the high byte's top bit then sign-extends).
   - Loaded narrower: a word stored, then read as its low byte and halfword,
     signed and unsigned (an AND, or a shift pair), across a join.

   Kept in memory, with outputs that depend on every byte: a word written and
   then a byte of it (a partial update of a wider field); a signed narrow
   field read wider. */
#include <stdio.h>
#include <stdint.h>

struct err2 { uint16_t error; };
union flag { uint8_t b; uint16_t h; uint32_t w; };
struct tag4 { uint8_t tag; } __attribute__((aligned(4)));
union bytes2 { uint8_t b[2]; uint16_t h; int16_t sh; };
union sflag { int8_t b; uint16_t h; };
union w8 { uint32_t w; uint8_t b; int8_t sb; uint16_t h; int16_t sh; };

__attribute__((noinline)) static struct err2 make_err(uint32_t k)
{
    struct err2 t;
    t.error = (uint16_t)(k * 3u + 1u);
    return t;
}

__attribute__((noinline)) static uint32_t flag_as_half(uint32_t k)
{
    union flag f;
    f.b = k > 4;
    return (f.h & 0xff) * 10 + f.b;
}

__attribute__((noinline)) static struct tag4 make_tag(uint32_t n)
{
    struct tag4 t;
    t.tag = (uint8_t)(n * 7u);
    return t;
}

__attribute__((noinline)) static uint32_t word_parts(uint32_t n, uint32_t k)
{
    union w8 f;
    f.w = n * 5u;
    if (k & 1)
        f.w = n + 0xf0f3u;
    return f.b + f.sb * 3 + f.h * 7 + f.sh * 11;
}

__attribute__((noinline)) static uint32_t partial_update(uint32_t n)
{
    union flag f;
    f.w = n;
    f.b = 7;
    return f.w;
}

__attribute__((noinline)) static uint32_t bytes_as_half(uint32_t n)
{
    union bytes2 x;
    x.b[0] = (uint8_t)n;
    x.b[1] = (uint8_t)(n >> 8);
    return x.h;
}

__attribute__((noinline)) static uint32_t bytes_as_signed_half(uint32_t n)
{
    union bytes2 x;
    x.b[0] = (uint8_t)n;
    x.b[1] = (uint8_t)(n >> 8);
    return (uint32_t)(int32_t)x.sh;
}

__attribute__((noinline)) static uint32_t word_as_byte(uint32_t n)
{
    union flag f;
    f.w = n * 5u;
    return f.b;
}

__attribute__((noinline)) static uint32_t signed_as_half(int32_t n)
{
    union sflag x;
    x.b = (int8_t)(n & 0x7f);
    return (x.h & 0xff) + (uint32_t)(int32_t)x.b;
}

int main(void)
{
    uint32_t r = 0;
    for (uint32_t k = 0; k < 9; k++)
    {
        struct err2 e = make_err(k * 1000u);
        r = r * 31 + e.error;
        r = r * 31 + flag_as_half(k);
        r = r * 31 + make_tag(k * 12345u).tag;
        r = r * 31 + word_parts(k * 0x10203fu + 0x7f80u, k);
        r = r * 31 + partial_update(k * 0x01020304u);
        r = r * 31 + bytes_as_half(k * 4099u);
        r = r * 31 + bytes_as_signed_half(k * 0x3907u);
        r = r * 31 + word_as_byte(k * 77u);
        r = r * 31 + signed_as_half((int32_t)k * 45 - 100);
    }
    printf("%u\n", (unsigned)r);
    return 0;
}
