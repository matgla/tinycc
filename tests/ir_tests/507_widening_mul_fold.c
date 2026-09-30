/* A widening multiply (UMULL/SMULL: 32x32 -> 64) with a constant operand
   folds like MUL does: by 0 to a 64-bit 0, and with both operands constant to
   the product -- reading each operand as its low word, zero-extended for
   UMULL and sign-extended for SMULL.  zig.h builds a u128 multiply from these
   products, and a u64 widened to u128 has a constant-zero high half, so its
   cross terms are exactly `x UMULL #0`. */
#include <stdio.h>
#include <stdint.h>

typedef struct { uint64_t lo, hi; } u128;

static inline uint64_t umull(uint32_t a, uint32_t b) { return (uint64_t)a * b; }
static inline int64_t smull(int32_t a, int32_t b) { return (int64_t)a * b; }

/* zig.h's 64x64->128: every step is a*b + c + d with c, d < 2^32. */
static inline u128 mul_wide(uint64_t l, uint64_t r)
{
    uint32_t l0 = (uint32_t)l, l1 = (uint32_t)(l >> 32);
    uint32_t r0 = (uint32_t)r, r1 = (uint32_t)(r >> 32);
    uint64_t p0 = (uint64_t)l0 * r0;
    uint64_t p1 = (uint64_t)l1 * r0 + (uint32_t)(p0 >> 32);
    uint64_t p2 = (uint64_t)l0 * r1 + (uint32_t)p1;
    uint64_t p3 = (uint64_t)l1 * r1 + (uint32_t)(p1 >> 32) + (uint32_t)(p2 >> 32);
    u128 res = { ((uint64_t)(uint32_t)p2 << 32) | (uint32_t)p0, p3 };
    return res;
}

/* The low 128 bits of a 128x128 product; hi halves of 0 leave x UMULL #0. */
static inline u128 mul128(u128 a, u128 b)
{
    u128 res = mul_wide(a.lo, b.lo);
    res.hi += a.lo * b.hi + a.hi * b.lo;
    return res;
}

__attribute__((noinline)) static uint64_t mum(uint64_t a, uint64_t b, uint64_t *hi)
{
    u128 x = { a, 0 }, y = { b, 0 };
    u128 r = mul128(x, y);
    *hi = r.hi;
    return r.lo;
}

/* Same with only one side widened: one cross term is still live. */
__attribute__((noinline)) static uint64_t half(uint64_t a, uint64_t bhi, uint64_t blo, uint64_t *hi)
{
    u128 x = { a, 0 }, y = { blo, bhi };
    u128 r = mul128(x, y);
    *hi = r.hi;
    return r.lo;
}

__attribute__((noinline)) static uint64_t zeros(uint32_t x, int32_t s)
{
    return umull(x, 0) + umull(0, x) + (uint64_t)smull(s, 0) + (uint64_t)smull(0, s) + 5;
}

__attribute__((noinline)) static void consts(void)
{
    printf("u %016llx %016llx %016llx\n", (unsigned long long)umull(0xffffffffu, 0xffffffffu),
           (unsigned long long)umull(0x80000000u, 2), (unsigned long long)umull(7, 9));
    printf("s %016llx %016llx %016llx\n", (unsigned long long)smull(-1, -1),
           (unsigned long long)smull(INT32_MIN, INT32_MIN), (unsigned long long)smull(-3, 5));
}

/* Reference: 64x64 -> 128 by 16-bit digits, no widening multiply shapes. */
static void ref_mul(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi)
{
    uint32_t r[8] = {0};
    for (int i = 0; i < 4; i++) {
        uint32_t carry = 0;
        uint32_t ai = (uint32_t)(a >> (16 * i)) & 0xffff;
        for (int j = 0; j < 4; j++) {
            uint32_t bj = (uint32_t)(b >> (16 * j)) & 0xffff;
            uint32_t t = ai * bj + r[i + j] + carry;
            r[i + j] = t & 0xffff;
            carry = t >> 16;
        }
        r[i + 4] = carry;
    }
    *lo = (uint64_t)r[0] | (uint64_t)r[1] << 16 | (uint64_t)r[2] << 32 | (uint64_t)r[3] << 48;
    *hi = (uint64_t)r[4] | (uint64_t)r[5] << 16 | (uint64_t)r[6] << 32 | (uint64_t)r[7] << 48;
}

int main(void)
{
    static const uint64_t v[] = {
        0, 1, 2, 0xffffffffull, 0x100000000ull, 0xffffffffffffffffull,
        0x9e3779b97f4a7c15ull, 0xa0761d6478bd642full, 0xe7037ed1a0b428dbull,
        0x8000000000000000ull, 0x00000000ffffffffull << 16,
    };
    int n = sizeof v / sizeof v[0], bad = 0;
    uint64_t acc = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            uint64_t hi, lo = mum(v[i], v[j], &hi), rlo, rhi;
            ref_mul(v[i], v[j], &rlo, &rhi);
            if (lo != rlo || hi != rhi) {
                printf("mum %d %d: %016llx %016llx want %016llx %016llx\n", i, j,
                       (unsigned long long)hi, (unsigned long long)lo,
                       (unsigned long long)rhi, (unsigned long long)rlo);
                bad++;
            }
            uint64_t h2, l2 = half(v[i], v[(i + j) % n], v[j], &h2);
            /* (a * (bhi:blo)) mod 2^128 = a*blo + (a*bhi << 64) */
            if (l2 != rlo || h2 != rhi + v[i] * v[(i + j) % n]) {
                printf("half %d %d\n", i, j);
                bad++;
            }
            acc ^= lo * 3 + hi;
        }
    printf("mul %s %016llx\n", bad ? "BAD" : "ok", (unsigned long long)acc);
    printf("zeros %llu %llu\n", (unsigned long long)zeros(0xdeadbeefu, -7),
           (unsigned long long)zeros(0, 0));
    consts();
    return 0;
}
