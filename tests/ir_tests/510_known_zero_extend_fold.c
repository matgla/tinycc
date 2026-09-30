/* `x & M` and `UBFX x,#0,#w` are copies of x when x cannot have a bit outside
   the mask (ssa:fold, fold_mask_is_copy).  The maybe-set-bits walk now knows
   that an unsigned byte/halfword load zero-extends, that a UBFX yields only
   its field, and that a STORE into a register temp copies its source -- so the
   `uxtb` a byte load, or a byte handed through an inlined Zig helper, used to
   carry is gone.  A signed narrow load sign-extends and keeps its mask, and so
   does a mask narrower than the value. */
#include <stdio.h>
#include <stdint.h>

typedef uint8_t meta_t;

static inline uint32_t fp_of(meta_t m) { return m & 0x7fu; }
static inline int used_of(meta_t m) { return (m >> 7) & 1; }

__attribute__((noinline)) static uint32_t byte_masks(const uint8_t *p, int n)
{
    uint32_t acc = 0;
    for (int i = 0; i < n; i++) {
        uint32_t b = p[i];
        acc += (b & 0xffu) + ((uint32_t)p[i] & 0x1ffu) + fp_of(p[i]) * 3u + (uint32_t)used_of(p[i]);
    }
    return acc;
}

__attribute__((noinline)) static uint32_t half_masks(const uint16_t *p, int n)
{
    uint32_t acc = 0;
    for (int i = 0; i < n; i++)
        acc += (p[i] & 0xffffu) + (p[i] & 0xffu) * 7u + ((uint32_t)p[i] >> 3 & 0x1fffu);
    return acc;
}

/* Signed loads sign-extend: the mask is real. */
__attribute__((noinline)) static uint32_t signed_masks(const int8_t *p, const int16_t *q, int n)
{
    uint32_t acc = 0;
    for (int i = 0; i < n; i++)
        acc += ((uint32_t)p[i] & 0xffu) + ((uint32_t)q[i] & 0xffffu) + ((uint32_t)p[i] & 0x7fu);
    return acc;
}

/* A byte field extracted with UBFX, then masked again. */
__attribute__((noinline)) static uint32_t fields(const uint32_t *w, int n)
{
    uint32_t acc = 0;
    for (int i = 0; i < n; i++) {
        uint32_t f = (w[i] >> 8) & 0xffu;
        acc += (f & 0xffu) + (f & 0x3fu) + ((uint8_t)f & 0xffu);
    }
    return acc;
}

/* A narrow local kept in a byte-typed variable across a call. */
__attribute__((noinline)) static uint32_t id(uint32_t x) { return x; }
__attribute__((noinline)) static uint32_t narrow_var(const uint8_t *p, int n)
{
    uint32_t acc = 0;
    for (int i = 0; i < n; i++) {
        volatile uint8_t keep = p[i];
        uint8_t b = keep;
        acc += id(b) + (b & 0xffu) + (uint8_t)(b + 200u);
    }
    return acc;
}

int main(void)
{
    uint8_t bytes[64];
    uint16_t halves[64];
    int8_t sb[64];
    int16_t sh[64];
    uint32_t words[64];
    uint32_t s = 0x2545f491u;
    for (int i = 0; i < 64; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        bytes[i] = (uint8_t)s;
        halves[i] = (uint16_t)(s >> 8);
        sb[i] = (int8_t)(s >> 16);
        sh[i] = (int16_t)(s >> 3);
        words[i] = s;
    }
    bytes[0] = 0xff; bytes[1] = 0x80; bytes[2] = 0; sb[0] = -1; sb[1] = -128; sh[0] = -32768;
    printf("byte %u\n", byte_masks(bytes, 64));
    printf("half %u\n", half_masks(halves, 64));
    printf("signed %u\n", signed_masks(sb, sh, 64));
    printf("fields %u\n", fields(words, 64));
    printf("narrow %u\n", narrow_var(bytes, 64));
    return 0;
}
