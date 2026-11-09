/*
 *  TCC Tests - PACK64 from stack stores must not cross into a join
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Zig's writePackedInt shape (device zig: every comptime f128 bitcast was 0).
 * pack64_from_stack_stores forwarded the fall-through arm's word STOREs into
 * an INT64 LOAD that was itself the join of the shift's three arms; it now
 * skips a LOAD that is a jump target.  ub_only_body_elide marked only the
 * base offset of an address-taken slot as initialised, so `tmp` filled via
 * `&tmp[i]` and copied out word by word read as uninit and the body was
 * emptied; it now blocks the whole object and each write's width. */
#include <stdint.h>
#include <stdio.h>

typedef struct { __attribute__((aligned(8))) uint64_t lo; uint64_t hi; } u128;
struct slice_u8 { uint8_t *ptr; uintptr_t len; };
struct arr16 { uint8_t array[16]; };

static inline u128 shr(u128 lhs, uint8_t rhs)
{
    if (rhs == 0) return lhs;
    if (rhs >= 64) return (u128){ .hi = 0, .lo = lhs.hi >> (rhs - 64) };
    return (u128){ .hi = lhs.hi >> rhs, .lo = lhs.hi << (64 - rhs) | lhs.lo >> rhs };
}

/* Little-endian bytes of `a2` at bit offset `a1` (a multiple of 8 here). */
__attribute__((noinline)) void write_le(struct slice_u8 const a0, uintptr_t const a1, u128 const a2)
{
    uint8_t *p = a0.ptr + a1 / 8u;
    u128 v = (u128){ .hi = a2.hi, .lo = a2.lo | (p[0] & 3) };
    struct arr16 tmp;
    for (uintptr_t i = 0; i < 16; i++)
        tmp.array[i] = (uint8_t)shr(v, (uint8_t)(i * 8u)).lo;
    *(struct arr16 *)p = tmp;
}

/* Straight-line: two word stores then the 8-byte load -- still a plain read. */
__attribute__((noinline)) uint64_t pair(uint32_t lo, uint32_t hi)
{
    union { uint32_t w[2]; uint64_t d; } u;
    u.w[0] = lo;
    u.w[1] = hi;
    return u.d;
}

static void dump(const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++)
        printf("%02x", b[i]);
    printf("\n");
}

int main(void)
{
    static const uintptr_t bits[] = { 0, 16, 24 };
    for (int k = 0; k < 3; k++) {
        uint8_t buf[24];
        for (int i = 0; i < 24; i++)
            buf[i] = 0;
        write_le((struct slice_u8){ buf, 24 }, bits[k],
                 (u128){ .hi = 0x7fff000000000000ull, .lo = 0x1122334455667788ull });
        dump(buf, 20);
    }
    printf("%016llx\n", (unsigned long long)pair(0x89abcdefu, 0x01234567u));
    return 0;
}
