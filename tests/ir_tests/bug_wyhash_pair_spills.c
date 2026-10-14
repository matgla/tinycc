/*
 *  TCC IR - Wyhash multiply pairs under register pressure
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdint.h>
#include <stdio.h>

typedef struct { __attribute__((aligned(8))) uint64_t lo; int64_t hi; } i128;
__attribute__((noinline)) i128 __multi3(i128 a, i128 b)
{
    uint32_t aw[4] = {(uint32_t)a.lo, (uint32_t)(a.lo >> 32), (uint32_t)a.hi, (uint32_t)((uint64_t)a.hi >> 32)};
    uint32_t bw[4] = {(uint32_t)b.lo, (uint32_t)(b.lo >> 32), (uint32_t)b.hi, (uint32_t)((uint64_t)b.hi >> 32)};
    uint32_t r[4] = {0};
    for (int i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (int j = 0; i + j < 4; j++) {
            uint64_t t = (uint64_t)aw[i] * bw[j] + r[i + j] + carry;
            r[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
    }
    i128 out = {r[0] | (uint64_t)r[1] << 32, (int64_t)(r[2] | (uint64_t)r[3] << 32)};
    return out;
}

__attribute__((noinline)) uint64_t wyhash_mum(uint64_t a, uint64_t b)
{
    i128 x = {a, 0}, y = {b, 0};
    i128 p = __multi3(x, y);
    return p.lo ^ (uint64_t)p.hi;
}

__attribute__((noinline)) uint64_t one_addend(uint32_t a, uint32_t b, uint32_t c)
{
    return (uint64_t)a * b + c;
}

__attribute__((noinline)) uint64_t two_addends(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return (uint64_t)a * b + c + d;
}

__attribute__((noinline)) uint64_t high_addend(uint32_t a, uint32_t b, uint64_t c)
{
    return (uint64_t)a * b + (c >> 32);
}

__attribute__((noinline)) uint64_t high_addends(uint32_t a, uint32_t b, uint64_t c, uint64_t d)
{
    return (uint64_t)a * b + (c >> 32) + (d >> 32);
}

__attribute__((noinline)) uint64_t masked_high_addend(uint32_t a, uint32_t b, uint64_t c)
{
    uint64_t masked = c & 0xffffffff00000000ull;
    return (uint64_t)a * b + (masked >> 32);
}

__attribute__((noinline)) uint64_t kept_pair_addend(uint32_t a, uint32_t b, uint64_t c,
                                                   volatile uint64_t *saved)
{
    uint64_t result = (uint64_t)a * b + (c >> 32);
    *saved = c;
    return result;
}

__attribute__((noinline)) uint64_t late_high_addend(uint32_t a, uint32_t b, uint32_t c, const volatile uint64_t *d)
{
    uint64_t t = (uint64_t)a * b + c;
    uint64_t last = *d;
    return t + (last >> 32);
}

__attribute__((noinline)) uint64_t signed_high_addend(uint32_t a, uint32_t b, int64_t c)
{
    return (uint64_t)a * b + (c >> 32);
}

__attribute__((noinline)) uint64_t shift31_addend(uint32_t a, uint32_t b, uint64_t c)
{
    return (uint64_t)a * b + (c >> 31);
}

static uint64_t ref_mum(uint64_t a, uint64_t b)
{
    uint32_t r[8] = {0};
    for (int i = 0; i < 4; i++) {
        uint32_t carry = 0, ai = (uint32_t)(a >> (16 * i)) & 0xffff;
        for (int j = 0; j < 4; j++) {
            uint32_t t = ai * ((uint32_t)(b >> (16 * j)) & 0xffff) + r[i + j] + carry;
            r[i + j] = t & 0xffff;
            carry = t >> 16;
        }
        r[i + 4] = carry;
    }
    uint64_t lo = r[0] | (uint64_t)r[1] << 16 | (uint64_t)r[2] << 32 | (uint64_t)r[3] << 48;
    uint64_t hi = r[4] | (uint64_t)r[5] << 16 | (uint64_t)r[6] << 32 | (uint64_t)r[7] << 48;
    return lo ^ hi;
}

static uint64_t read64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

__attribute__((noinline)) void wyhash_rounds(uint64_t *state, const unsigned char *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++, p += 48) {
        for (int lane = 0; lane < 3; lane++) {
            uint64_t a = read64(p + 16 * lane) ^ 0xe7037ed1a0b428dbull;
            uint64_t b = read64(p + 16 * lane + 8) ^ state[lane];
            i128 x = {a, 0}, y = {b, 0};
            i128 r = __multi3(x, y);
            state[lane] = r.lo ^ (uint64_t)r.hi;
        }
    }
}

int main(void)
{
    static const uint64_t edges[] = {0, 1, 0xffffffffull, 0x100000000ull,
        0xffffffffffffffffull, 0x8000000000000000ull, 0x9e3779b97f4a7c15ull};
    uint64_t values[64];
    uint32_t rng = 0x12345678u;
    int bad = 0;
    for (int i = 0; i < 64; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        uint32_t hi = rng;
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        values[i] = i < 7 ? edges[i] : (uint64_t)hi << 32 | rng;
    }
    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 64; j++) {
            uint32_t a = values[i], b = values[j], c = values[i] >> 32, d = values[j] >> 32;
            bad += wyhash_mum(values[i], values[j]) != ref_mum(values[i], values[j]);
            bad += one_addend(a, b, c) != (uint64_t)a * b + c;
            bad += two_addends(a, b, c, d) != (uint64_t)a * b + c + d;
            uint64_t product = (uint64_t)(a & 0xffff) * b + ((uint64_t)(a >> 16) * b << 16);
            volatile uint64_t last = values[j];
            bad += high_addend(a, b, values[i]) != product + c;
            bad += high_addends(a, b, values[i], values[j]) != product + c + d;
            bad += masked_high_addend(a, b, values[i]) != product + c;
            volatile uint64_t saved = 0;
            bad += kept_pair_addend(a, b, values[i], &saved) != product + c;
            bad += saved != values[i];
            bad += late_high_addend(a, b, c, &last) != product + c + d;
            bad += signed_high_addend(a, b, (int64_t)values[i]) != product + (int64_t)(int32_t)c;
            bad += shift31_addend(a, b, values[i]) != product + 2ull * c + ((a >> 31) & 1);
        }
    }
    unsigned char bytes[48 * 7 + 8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (unsigned char)(i * 73 + (i >> 3));
    for (int offset = 0; offset < 8; offset++) {
        uint64_t state[3] = {edges[4], edges[5], edges[6]};
        uint64_t expected[3] = {edges[4], edges[5], edges[6]};
        wyhash_rounds(state, bytes + offset, 7);
        for (int i = 0; i < 7; i++) {
            for (int lane = 0; lane < 3; lane++) {
                const unsigned char *p = bytes + offset + 48 * i + 16 * lane;
                expected[lane] = ref_mum(read64(p) ^ 0xe7037ed1a0b428dbull,
                                         read64(p + 8) ^ expected[lane]);
            }
        }
        for (int lane = 0; lane < 3; lane++)
            bad += state[lane] != expected[lane];
    }
    printf("wyhash pairs %s\n", bad ? "BAD" : "ok");
    return bad != 0;
}
