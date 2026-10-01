/* A STORE of a byte or halfword into a register temporary is a whole
   definition: codegen writes the register with a MOV, or the temp's spill
   slot with a word store of the extended value, so nothing of the old value
   survives.  The allocator used to call it a partial def, which made every
   byte temp an inlined helper leaves behind live from function entry: across
   every call, each pinned a callee-saved register, and std.HashMap's probe
   loop (the Zig C backend's shape, below) had its index, count and metadata
   pointer pushed to the stack.  The shapes here: byte temps in a probe loop
   around a call, halfword temps, and more byte temps live across a call than
   there are callee-saved registers, so some of them spill. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

typedef uint8_t meta_t;
struct map { meta_t *metadata; uint32_t *keys; uint32_t cap; };

static bool is_used(meta_t const a0)
{
    meta_t t0;
    memset(&t0, 0, sizeof t0);
    t0 = a0;
    return (*(uint8_t const *)&t0 >> 7) == 1;
}
static bool is_free(meta_t const a0) { return a0 == 0; }

__attribute__((noinline)) static bool key_eql(uint32_t a, uint32_t b) { return a == b; }

__attribute__((noinline)) static int get_index(struct map const a0, uint32_t key)
{
    struct map t0 = a0;
    struct map const *t1 = &t0;
    uint32_t h = key * 2654435761u;
    uint8_t fp = (uint8_t)(h >> 25);
    uint32_t mask = t1->cap - 1, limit = t1->cap, idx = h & mask;
    meta_t *m = t1->metadata + idx;
    for (;;) {
        meta_t b = *m;
        if (is_free(b) || limit == 0)
            return -1;
        if (is_used(*m) && (*m & 0x7f) == fp && key_eql(t1->keys[idx], key))
            return (int)idx;
        limit--;
        idx = (idx + 1) & mask;
        m = t1->metadata + idx;
    }
}

__attribute__((noinline)) static void put(struct map *mp, uint32_t key)
{
    uint32_t h = key * 2654435761u, mask = mp->cap - 1, idx = h & mask;
    while (mp->metadata[idx] != 0)
        idx = (idx + 1) & mask;
    mp->metadata[idx] = (uint8_t)(0x80 | (h >> 25));
    mp->keys[idx] = key;
}

__attribute__((noinline)) static uint32_t sink(uint32_t x) { return x * 3 + 1; }

/* Twelve byte temps and four halfword temps, all live across calls. */
__attribute__((noinline)) static uint32_t many(const uint8_t *p, const uint16_t *q)
{
    uint8_t a = p[0], b = p[1], c = p[2], d = p[3], e = p[4], f = p[5];
    uint8_t g = p[6], h = p[7], i = p[8], j = p[9], k = p[10], l = p[11];
    uint16_t w = q[0], x = q[1], y = q[2], z = q[3];
    uint32_t acc = sink(a + b);
    acc += sink(c ^ d) + sink((uint32_t)e * f);
    acc ^= sink(g) + h + sink(i + j);
    acc += sink(w) ^ x;
    acc += (uint32_t)k * l + sink(y + z);
    return acc + a + b + c + d + e + f + g + h + i + j + k + l + w + x + y + z;
}

int main(void)
{
    static uint8_t meta[64];
    static uint32_t keys[64];
    struct map m = { meta, keys, 64 };
    for (uint32_t k = 1; k <= 40; k++)
        put(&m, k * 7919u);
    int found = 0, sum = 0, missing = 0;
    for (uint32_t k = 1; k <= 60; k++) {
        int i = get_index(m, k * 7919u);
        if (i >= 0) { found++; sum += i; }
        else missing++;
    }
    printf("probe found=%d missing=%d sum=%d\n", found, missing, sum);

    uint8_t bytes[12];
    uint16_t halves[4];
    for (int i = 0; i < 12; i++) bytes[i] = (uint8_t)(0xf0 + i * 7);
    for (int i = 0; i < 4; i++) halves[i] = (uint16_t)(0xfff0 - i * 911);
    printf("many %u\n", many(bytes, halves));
    return 0;
}
