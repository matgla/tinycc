/* The Zig C backend writes a loop counter through two locals -- `t6 = t10;
   ... t6 = t6 + 1; t10 = t6;` -- which hid every Zig loop's induction
   variable (ssa:loop_unroll now reads the pair as one counter).  A
   constant-trip loop whose body loads and stores now unrolls, and a table it
   indexes by the counter folds to the table's values once the index is
   constant (ssa:unroll_cascade):
   MultiArrayList.slice becomes straight-line stores.  Beside those: an
   indexed access whose shift was fused (barrel_shifts, carried into every
   copy), a body binding a load to an inlined parameter (kept as a loop), a
   packed bitfield RMW, 64-bit and 128-bit values, a stack array written at a
   runtime index before the unrolled loop reads it, and a counter copy read
   after the loop. */
#include <stdio.h>
#include <stdint.h>

struct arr8 { uintptr_t array[8]; };
struct parr8 { uint8_t *array[8]; };
struct mal { uint8_t *bytes; uintptr_t len, capacity; };
struct slice { struct parr8 ptrs; uintptr_t len, capacity; };

__attribute__((noinline)) static struct slice slice_fn(struct mal const a0)
{
    struct mal const *t1;
    uintptr_t *t4, t6, t13, t14, t10;
    uintptr_t const *t5;
    uint8_t *t9, *t7, **t15;
    uint32_t t11;
    struct slice t16, t2;
    struct mal t0;
    _Bool t12;
    t0 = a0; t1 = &t0;
    t4 = &t2.len; t5 = &t1->len; t6 = *t5; *t4 = t6;
    t4 = &t2.capacity; t5 = &t1->capacity; t6 = *t5; *t4 = t6;
    t9 = t1->bytes; t7 = t9; t10 = 0;
zig_loop_22:
    t6 = t10; t11 = (uint32_t)t6; t12 = t11 < 8u;
    if (t12) {
        t13 = (struct arr8){{4, 4, 4, 4, 4, 4, 4, 2}}.array[t6];
        t14 = (struct arr8){{7, 6, 5, 4, 3, 2, 1, 0}}.array[t6];
        t15 = &t2.ptrs.array[t14]; t9 = t7; *t15 = t9; t9 = t7;
        t5 = &t1->capacity; t14 = *t5; t14 = t13 * t14; t9 = t9 + t14; t7 = t9;
        goto zig_block_1;
    }
    goto zig_block_0;
zig_block_1:;
    t6 = t6 + 1; t10 = t6; goto zig_loop_22;
zig_block_0:;
    t16 = t2;
    return t16;
}

/* Zig's counter shape, counter read after the loop, sum of a const table. */
static const uint16_t weights[6] = {3, 1000, 7, 65535, 12, 9};
__attribute__((noinline)) static uint32_t zig_sum(uint32_t scale, uint32_t *last)
{
    uintptr_t t6, t10 = 0;
    uint32_t acc = 0;
loop:
    t6 = t10;
    if ((uint32_t)t6 < 6u) {
        acc += weights[t6] * scale + (uint32_t)t6;
        t6 = t6 + 1; t10 = t6; goto loop;
    }
    *last = (uint32_t)t10;
    return acc;
}

typedef struct { uint64_t lo, hi; } u128;
static inline u128 mul64(uint64_t a, uint64_t b)
{
    uint64_t p0 = (uint64_t)(uint32_t)a * (uint32_t)b, p1 = (a >> 32) * (uint32_t)b;
    uint64_t p2 = (uint64_t)(uint32_t)a * (b >> 32), p3 = (a >> 32) * (b >> 32);
    uint64_t mid = (p0 >> 32) + (uint32_t)p1 + (uint32_t)p2;
    u128 r = {(mid << 32) | (uint32_t)p0, p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32)};
    return r;
}

/* 128-bit values stored by index, read back reversed with a fused shift. */
__attribute__((noinline)) static uint64_t wide(const uint64_t *v)
{
    u128 w[8];
    for (int k = 0; k < 8; k++)
        w[k] = mul64(v[k], v[(k + 3) & 7]);
    uint64_t acc = 0;
    for (int k = 0; k < 8; k++)
        acc = acc * 31 + (w[k].lo ^ (w[7 - k].hi >> 1));
    return acc;
}

static uint32_t mix(uint32_t h, uint32_t v) { return (h ^ (v + 0x9e3779b9u + (h << 6) + (h >> 2))) * 2654435761u; }

/* A runtime-index store into a stack array, then an unrolled read loop,
   with the element bound to an inlined parameter. */
__attribute__((noinline)) static uint32_t alias(uint32_t idx, uint32_t val)
{
    uint32_t a[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    a[idx & 7] = val;
    uint32_t h = 1;
    for (uint32_t k = 0; k < 8; k++)
        h = mix(h, a[k]);
    return h;
}

struct bf { unsigned b0 : 1, b1 : 5, b2 : 6, b3 : 11, b4 : 5; } __attribute__((packed));
__attribute__((noinline)) static uint32_t bits(uint32_t s)
{
    struct bf x = {0, 0, 0, 0, 0};
    x.b1 = 19;
    for (unsigned k = 0; k < 7; k++)
        x.b2 = (s + k) & 63;
    return x.b1 * 1000 + x.b2;
}

int main(void)
{
    static uint8_t mem[512];
    struct slice sl = slice_fn((struct mal){mem, 3, 5});
    uint32_t acc = 0;
    for (int k = 0; k < 8; k++)
        acc = acc * 31 + (uint32_t)(sl.ptrs.array[k] - mem);
    printf("slice %u %u %u\n", acc, (unsigned)sl.len, (unsigned)sl.capacity);

    uint32_t last = 0;
    uint32_t s = zig_sum(3, &last);
    printf("zig_sum %u %u\n", s, last);

    uint64_t v[8];
    uint32_t r = 0x2545f491u;
    for (int i = 0; i < 8; i++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        v[i] = (uint64_t)r << 32 | (r * 2654435761u);
    }
    printf("wide %016llx\n", (unsigned long long)wide(v));
    printf("alias %08x %08x\n", alias(3, 7), alias(12, 99));
    printf("bits %u %u\n", bits(5), bits(60));
    return 0;
}
