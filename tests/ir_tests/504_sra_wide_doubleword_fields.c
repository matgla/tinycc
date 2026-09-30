/* sra: a 64-bit field of a frame object becomes a VAR, and one also touched a
   word at a time -- the Zig C backend's zig_u128, zero-filled and copied a word
   at a time, its halves read and written whole -- becomes two word VARs whose
   64-bit reads are PACK64s and whose 64-bit writes are split.  ssa:sccp folds a
   PACK64 of constants, and ssa:fold cancels a split taken apart again.  Every
   shape here is checked against values computed with plain uint64_t. */
#include <stdio.h>
#include <stdint.h>

typedef struct { uint64_t lo; uint64_t hi; } u128;

#define MK(h, l) ((u128){ .hi = (h), .lo = (l) })

static inline u128 u128_from64(uint64_t x) { return MK(0, x); }
static inline u128 u128_and(u128 a, u128 b) { return MK(a.hi & b.hi, a.lo & b.lo); }
static inline u128 u128_xor(u128 a, u128 b) { return MK(a.hi ^ b.hi, a.lo ^ b.lo); }
static inline u128 u128_shr(u128 a, unsigned n)
{
    if (n == 0) return a;
    if (n >= 64) return MK(0, a.hi >> (n - 64));
    return MK(a.hi >> n, a.hi << (64 - n) | a.lo >> n);
}
static inline u128 u128_shl(u128 a, unsigned n)
{
    if (n == 0) return a;
    if (n >= 64) return MK(a.lo << (n - 64), 0);
    return MK(a.hi << n | a.lo >> (64 - n), a.lo << n);
}
static inline u128 u128_max(unsigned bits) { return u128_shr(MK(~0ull, ~0ull), 128 - bits); }
static inline u128 u128_trunc(u128 a, unsigned bits) { return u128_and(a, u128_max(bits)); }
static inline uint64_t u64_trunc(u128 a) { return a.lo; }

/* By value and out of line, like __multi3: its arguments stay in memory. */
__attribute__((noinline)) static u128 mul_full(u128 a, u128 b)
{
    uint64_t al = (uint32_t)a.lo, ah = a.lo >> 32, bl = (uint32_t)b.lo, bh = b.lo >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    u128 r = MK(hh + (lh >> 32) + (hl >> 32) + (mid >> 32), (mid << 32) | (uint32_t)ll);
    r.hi += a.lo * b.hi + a.hi * b.lo;
    return r;
}

/* wyhash's mum: the shape the Zig compiler runs 3 million times. */
static inline void mum(uint64_t *a, uint64_t *b)
{
    u128 x = u128_from64(*a), y = u128_from64(*b);
    u128 p = u128_trunc(mul_full(x, y), 128);
    *a = u64_trunc(p);
    p = u128_shr(p, 64);
    *b = u64_trunc(p);
}

/* A doubleword read a word at a time, then whole, across a branch. */
__attribute__((noinline)) static uint64_t words_then_whole(uint64_t v, int c)
{
    u128 s = MK(v * 3, v ^ 0x0123456789abcdefull);
    uint32_t w0, w1;
    __builtin_memcpy(&w0, (char *)&s.lo, 4);
    __builtin_memcpy(&w1, (char *)&s.lo + 4, 4);
    if (c)
        s.lo = ((uint64_t)w0 << 32) | w1;
    else
        s.hi = 0xfedcba9876543210ull;
    return s.lo + s.hi;
}

/* A loop-carried u128 accumulator: phis over both split words. */
__attribute__((noinline)) static u128 accumulate(uint64_t seed, int n)
{
    u128 acc = MK(seed, ~seed);
    for (int i = 0; i < n; i++) {
        acc = u128_xor(acc, u128_shl(u128_from64(seed + i), (unsigned)(i * 7) & 127));
        if (acc.lo & 1)
            acc = u128_shr(acc, 3);
    }
    return acc;
}

/* 64-bit fields only: one 64-bit VAR each; compare, pass, return them. */
typedef struct { uint64_t a; uint64_t b; } pair64;
__attribute__((noinline)) static uint64_t take64(uint64_t x) { return x * 5 + 1; }
__attribute__((noinline)) static uint64_t pure64(uint64_t x, uint64_t y)
{
    pair64 p;
    p.a = x;
    p.b = 0x8000000000000001ull;
    if (p.a > y)
        p.b = take64(p.a) + y;
    else
        p.a = p.b ^ y;
    return p.a < p.b ? p.b - p.a : take64(p.a - p.b);
}

/* A double shares the object: that doubleword stays in memory. */
typedef struct { uint64_t u; double d; } mixed;
__attribute__((noinline)) static uint64_t with_double(uint64_t x)
{
    mixed m;
    m.u = x;
    m.d = (double)(x & 0xffff) * 0.5;
    m.u += (uint64_t)m.d;
    return m.u;
}

int main(void)
{
    uint64_t s = 0x9E3779B97F4A7C15ull, sum = 0;
    for (int i = 0; i < 64; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        uint64_t a = s, b = (i & 1) ? s >> 17 : ~s * 3;
        mum(&a, &b);
        sum += a ^ (b * 7);
        sum += words_then_whole(s, i & 2);
        u128 acc = accumulate(s, i & 15);
        sum ^= acc.lo + acc.hi * 11;
        sum += pure64(s, s >> (i & 7));
        sum += with_double(s);
    }
    u128 t = u128_trunc(MK(0x1111222233334444ull, 0x5555666677778888ull), 72);
    printf("%016llx %016llx\n", (unsigned long long)t.hi, (unsigned long long)t.lo);
    printf("%016llx\n", (unsigned long long)sum);
    return 0;
}
