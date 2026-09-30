/* A 32x32->64 product plus one or two zero-extended words is fused into
   UMAAL right before register allocation (source/opt/ra/umaal_fusion.c):
   {hi:lo} = a * b + c + d cannot overflow.  The destination inherits the
   accumulator's register pair, so the emitter must never let the addend
   moves clobber a multiplicand, and every pass after the fusion must see the
   accumulator (the fourth operand) as a read of both of its words.  Shapes:
   one addend, two addends in either order, addends taken from a pair's low
   and high words, loads, constants, a loop-carried bignum row, and enough
   live 128-bit products at once to force the operands onto the stack. */
#include <stdio.h>
#include <stdint.h>

typedef struct { uint64_t lo, hi; } u128;

static uint32_t rs = 0x12345678u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

/* Reference: schoolbook by 16-bit digits, nothing a UMULL can match. */
static void ref(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi)
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
    *lo = r[0] | (uint64_t)r[1] << 16 | (uint64_t)r[2] << 32 | (uint64_t)r[3] << 48;
    *hi = r[4] | (uint64_t)r[5] << 16 | (uint64_t)r[6] << 32 | (uint64_t)r[7] << 48;
}

static inline u128 wide(uint64_t l, uint64_t r)
{
    uint32_t l0 = (uint32_t)l, l1 = (uint32_t)(l >> 32), r0 = (uint32_t)r, r1 = (uint32_t)(r >> 32);
    uint64_t p0 = (uint64_t)l0 * r0;
    uint64_t p1 = (uint64_t)l1 * r0 + (uint32_t)(p0 >> 32);
    uint64_t p2 = (uint64_t)l0 * r1 + (uint32_t)p1;
    uint64_t p3 = (uint64_t)l1 * r1 + (uint32_t)(p1 >> 32) + (uint32_t)(p2 >> 32);
    u128 x = { ((uint64_t)(uint32_t)p2 << 32) | (uint32_t)p0, p3 };
    return x;
}

/* The addends in the other order, and added the other way round. */
static inline u128 wide_swapped(uint64_t l, uint64_t r)
{
    uint32_t l0 = (uint32_t)l, l1 = (uint32_t)(l >> 32), r0 = (uint32_t)r, r1 = (uint32_t)(r >> 32);
    uint64_t p0 = (uint64_t)r0 * l0;
    uint64_t p1 = (uint32_t)(p0 >> 32) + (uint64_t)r0 * l1;
    uint64_t p2 = (uint32_t)p1 + (uint64_t)r1 * l0;
    uint64_t p3 = (uint32_t)(p2 >> 32) + ((uint32_t)(p1 >> 32) + (uint64_t)l1 * r1);
    u128 x = { ((uint64_t)(uint32_t)p2 << 32) | (uint32_t)p0, p3 };
    return x;
}

__attribute__((noinline)) static uint64_t mum(uint64_t a, uint64_t b)
{
    u128 x = wide(a, b);
    return x.lo ^ x.hi;
}

__attribute__((noinline)) static void full(uint64_t a, uint64_t b, u128 *o, u128 *o2)
{
    *o = wide(a, b);
    *o2 = wide_swapped(a, b);
}

/* Multiplicand reused as an addend, and a constant addend. */
__attribute__((noinline)) static uint64_t self_add(uint32_t a, uint32_t b)
{
    return (uint64_t)a * b + a + (uint32_t)0xffffffffu;
}

/* Addends straight from memory. */
__attribute__((noinline)) static uint64_t mem_add(const uint32_t *p, uint32_t a, uint32_t b)
{
    return (uint64_t)a * b + p[0] + p[1];
}

/* r[0..n] += a[0..n) * b: the bignum row, carry loop-carried. */
__attribute__((noinline)) static uint32_t row(uint32_t *r, const uint32_t *a, int n, uint32_t b)
{
    uint32_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t = (uint64_t)a[i] * b + carry + r[i];
        r[i] = (uint32_t)t;
        carry = (uint32_t)(t >> 32);
    }
    return carry;
}

/* Eight products alive at once: more pairs than there are registers. */
__attribute__((noinline)) static uint64_t pressure(const uint64_t *v)
{
    u128 w[8];
    for (int k = 0; k < 8; k++)
        w[k] = wide(v[k], v[(k + 3) & 7]);
    uint64_t acc = 0;
    for (int k = 0; k < 8; k++)
        acc = acc * 31 + (w[k].lo ^ (w[7 - k].hi >> 1));
    return acc;
}

static uint64_t pressure_ref(const uint64_t *v)
{
    uint64_t lo[8], hi[8];
    for (int k = 0; k < 8; k++)
        ref(v[k], v[(k + 3) & 7], &lo[k], &hi[k]);
    uint64_t acc = 0;
    for (int k = 0; k < 8; k++)
        acc = acc * 31 + (lo[k] ^ (hi[7 - k] >> 1));
    return acc;
}

int main(void)
{
    static const uint64_t edge[] = {0, 1, 0xffffffffull, 0x100000000ull, 0xffffffffffffffffull,
                                    0x9e3779b97f4a7c15ull, 0x8000000000000000ull};
    int bad = 0, n = 0;
    uint64_t v[64];
    for (int i = 0; i < 64; i++)
        v[i] = i < 7 ? edge[i] : ((uint64_t)rnd() << 32 | rnd());
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 64; j++, n++) {
            uint64_t lo, hi;
            u128 x, y;
            ref(v[i], v[j], &lo, &hi);
            full(v[i], v[j], &x, &y);
            if (x.lo != lo || x.hi != hi || y.lo != lo || y.hi != hi || mum(v[i], v[j]) != (lo ^ hi))
                bad++;
            uint32_t a = (uint32_t)v[i], b = (uint32_t)v[j];
            if (self_add(a, b) != (uint64_t)a * b + a + 0xffffffffull)
                bad++;
            uint32_t m[2] = {(uint32_t)(v[i] >> 32), (uint32_t)(v[j] >> 32)};
            if (mem_add(m, a, b) != (uint64_t)a * b + m[0] + m[1])
                bad++;
        }
    printf("products %d %s\n", n, bad ? "BAD" : "ok");

    /* bignum: (sum a[i] 2^32i) * b, checked against 16-bit digits */
    uint32_t A[9], R[10], R2[10];
    for (int i = 0; i < 9; i++)
        A[i] = i < 2 ? 0xffffffffu : rnd();
    for (int t = 0; t < 20; t++) {
        uint32_t b = t ? rnd() : 0xffffffffu;
        for (int i = 0; i < 10; i++)
            R[i] = R2[i] = t & 1 ? rnd() : 0xffffffffu;
        R[9] = R2[9] = 0;
        uint32_t c = row(R, A, 9, b);
        R[9] += c;
        /* reference: R2 += A * b, 16-bit digits */
        uint32_t carry = 0;
        for (int i = 0; i < 18; i++) {
            uint32_t ad = (A[i / 2] >> (16 * (i & 1))) & 0xffff;
            uint32_t rd = (R2[i / 2] >> (16 * (i & 1))) & 0xffff;
            uint32_t s = ad * (b & 0xffff) + rd + carry;
            carry = s >> 16;
            R2[i / 2] = (R2[i / 2] & ~(0xffffu << (16 * (i & 1)))) | (s & 0xffff) << (16 * (i & 1));
            /* the b-high half lands one digit up: add it in a second sweep */
        }
        R2[9] += carry;
        carry = 0;
        for (int i = 1; i < 20; i++) {
            uint32_t ad = i - 1 < 18 ? (A[(i - 1) / 2] >> (16 * ((i - 1) & 1))) & 0xffff : 0;
            uint32_t rd = (R2[i / 2] >> (16 * (i & 1))) & 0xffff;
            uint32_t s = ad * (b >> 16) + rd + carry;
            carry = s >> 16;
            R2[i / 2] = (R2[i / 2] & ~(0xffffu << (16 * (i & 1)))) | (s & 0xffff) << (16 * (i & 1));
        }
        for (int i = 0; i < 10; i++)
            if (R[i] != R2[i])
                bad++;
    }
    printf("row %s\n", bad ? "BAD" : "ok");

    uint64_t pv[8];
    for (int i = 0; i < 8; i++)
        pv[i] = v[5 + i * 7];
    printf("pressure %s %016llx\n", pressure(pv) == pressure_ref(pv) ? "ok" : "BAD",
           (unsigned long long)pressure(pv));
    return 0;
}
