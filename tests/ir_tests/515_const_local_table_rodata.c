/* A local array or struct written only by one run of constant stores covering
   all of it, and only ever read through its address, is read from an image
   in .rodata (const_local_table) -- the Zig C backend's
   `(struct arr_8_usize){{4,4,...,2}}.array[i]` in every MultiArrayList.slice
   no longer rebuilds the table on each loop turn.  Beside the shapes it
   rewrites (word, byte, halfword and 64-bit tables; a table built once
   outside the loop), the ones it must leave alone: an element written by a
   later store or through a pointer, the address passed to a call or
   compared, a partly initialised object, and two tables sharing a slot in
   sibling scopes. */
#include <stdio.h>
#include <stdint.h>

struct arr8 { uintptr_t array[8]; };
struct parr8 { uint8_t *array[8]; };
struct mal { uint8_t *bytes; uintptr_t len, capacity; };
struct slice { struct parr8 ptrs; uintptr_t len, capacity; };

/* The Zig C backend's MultiArrayList.slice, as emitted. */
__attribute__((noinline)) static struct slice slice_fn(struct mal const a0)
{
    struct mal const *t1;
    struct parr8 *t3;
    uintptr_t *t4, t6, t13, t14, t10;
    uintptr_t const *t5;
    uint8_t *const *t8;
    uint8_t *t9, *t7, **t15;
    uint32_t t11;
    struct slice t16, t2;
    struct mal t0;
    _Bool t12;
    t0 = a0; t1 = &t0;
    t3 = &t2.ptrs; t4 = &t2.len; t5 = &t1->len; t6 = *t5; *t4 = t6;
    t4 = &t2.capacity; t5 = &t1->capacity; t6 = *t5; *t4 = t6;
    t8 = &t1->bytes; t9 = *t8; t7 = t9; t10 = 0;
zig_loop_22:
    t6 = t10; t11 = (uint32_t)t6; t12 = t11 < 8u;
    if (t12) {
        t13 = (struct arr8){{4, 4, 4, 4, 4, 4, 4, 2}}.array[t6];
        t14 = (struct arr8){{7, 6, 5, 4, 3, 2, 1, 0}}.array[t6];
        t3 = &t2.ptrs; t15 = &t3->array[t14]; t9 = t7; *t15 = t9; t9 = t7;
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

__attribute__((noinline)) static uint32_t bytes_halves(unsigned i)
{
    uint32_t s = 0;
    for (unsigned k = 0; k < 12; k++) {
        const uint8_t b[12] = {9, 250, 3, 7, 1, 0, 128, 64, 33, 17, 5, 2};
        const int16_t h[6] = {-3, 1000, -32768, 7, 0, 12345};
        s = s * 7 + b[(k + i) % 12] + (uint32_t)h[(k * i) % 6];
    }
    return s;
}

__attribute__((noinline)) static uint64_t wide(unsigned i)
{
    const uint64_t t[4] = {0x123456789abcdef0ull, 1, 0xffffffff00000000ull, 42};
    uint64_t s = 0;
    for (unsigned k = 0; k < 9; k++)
        s += t[(k + i) & 3] >> (k & 7);
    return s;
}

/* Must stay on the stack: written again after its initialiser. */
__attribute__((noinline)) static int rewritten(unsigned i, unsigned j)
{
    int t[4] = {1, 2, 3, 4};
    t[2] = 30;
    int *p = &t[0];
    p[i & 3] += 100;
    return t[j & 3] * 1000 + t[(j + 1) & 3];
}

__attribute__((noinline)) static int sum_of(const int *p, int n)
{
    int s = 0;
    for (int k = 0; k < n; k++)
        s += p[k];
    return s;
}

/* Must stay on the stack: its address leaves, and is compared. */
__attribute__((noinline)) static int escapes(unsigned i)
{
    const int t[5] = {5, 10, 15, 20, 25};
    const int *p = &t[i % 5];
    return sum_of(t, 5) + *p + (p == &t[2]);
}

/* Partly initialised: the rest is zero. */
struct part { uint32_t a; uint8_t c; uint32_t w[3]; };
__attribute__((noinline)) static uint32_t partial(unsigned i)
{
    struct part s = {7, 1};
    const uint32_t *w = s.w;
    return s.a + s.c + w[i % 3];
}

/* Two tables that may share a slot, in sibling scopes. */
__attribute__((noinline)) static int siblings(unsigned i, int which)
{
    int r = 0;
    if (which) {
        const int a[4] = {11, 12, 13, 14};
        r = a[i & 3];
    } else {
        const int b[4] = {21, 22, 23, 24};
        r = b[i & 3];
    }
    return r;
}

int main(void)
{
    static uint8_t mem[256];
    struct mal m = {mem, 3, 5};
    struct slice sl = slice_fn(m);
    unsigned acc = 0;
    for (int k = 0; k < 8; k++)
        acc = acc * 31 + (unsigned)(sl.ptrs.array[k] - mem);
    printf("slice %u %u %u\n", acc, (unsigned)sl.len, (unsigned)sl.capacity);
    printf("bytes %u %u\n", bytes_halves(0), bytes_halves(5));
    printf("wide %llu %llu\n", (unsigned long long)wide(0), (unsigned long long)wide(3));
    printf("rewritten %d %d\n", rewritten(1, 1), rewritten(2, 3));
    printf("escapes %d %d\n", escapes(2), escapes(4));
    printf("partial %u %u\n", partial(0), partial(2));
    printf("siblings %d %d %d\n", siblings(1, 1), siblings(2, 0), siblings(7, 1));
    return 0;
}
