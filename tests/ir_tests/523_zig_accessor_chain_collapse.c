/* The Zig compiler's InternPool accessor chain, as its C backend writes it.
   MultiArrayList.slice is a loop over the element's fields whose counter the
   backend spells with two locals and passes through an inlined
   zig_u32_bitCast_u32 before the exit test; once ssa:loop_unroll sees the
   counter through that, the loop unrolls, its per-field tables fold, and the
   body -- over 512 tokens of source, a few ops once optimized -- inlines as a
   small leaf into its callers, including one whose table addresses were
   hoisted into registers first and one that does an acquire load (which must
   not make the table reads look volatile).  Also: __atomic_load_n, and ordered
   and relaxed loads still re-read memory that changed in between; and a
   post-increment index read by a store in an unrolled body, and an unrolled
   store whose index went through a fused barrel shift. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
/* What tcc's <stdatomic.h> spells atomic_load_explicit as (the IR test
   environment puts newlib's header first, and a device has no relative
   path to tcc's). */
#ifndef __ATOMIC_ACQUIRE
#define __ATOMIC_RELAXED 0
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_SEQ_CST 5
#endif
#define memory_order_acquire __ATOMIC_ACQUIRE
#define atomic_load_explicit(object, order) __atomic_load_n((object), (order))

static inline uint32_t zig_u32_truncate_u32(uint32_t arg, uint8_t bits)
{
    return arg & (bits < UINT8_C(32) ? ((UINT32_C(1) << bits) - UINT32_C(1)) : UINT32_C(0xffffffff));
}
static inline uint32_t zig_u32_bitCast_u32(uint32_t arg, uint8_t bits) { return zig_u32_truncate_u32(arg, bits); }

struct arr_2_ptr_u8 { uint8_t *array[2]; };
struct arr_1_ptr_u8 { uint8_t *array[1]; };
struct arr_2_usize { uintptr_t array[2]; };
struct arr_1_usize { uintptr_t array[1]; };
struct MAL { uint8_t *bytes; uintptr_t len; uintptr_t capacity; };
struct Slice2 { struct arr_2_ptr_u8 ptrs; uintptr_t len; uintptr_t capacity; };
struct Slice1 { struct arr_1_ptr_u8 ptrs; uintptr_t len; uintptr_t capacity; };
struct slice_u32 { uint32_t *ptr; uintptr_t len; };
struct slice_u8 { uint8_t *ptr; uintptr_t len; };

/* Two fields: a u8 tag array after a u32 data array, larger field first. */
static struct Slice2 slice2(struct MAL const a0)
{
    struct MAL const *t1;
    struct arr_2_ptr_u8 *t3;
    uintptr_t *t4;
    uintptr_t const *t5;
    uintptr_t t6;
    uintptr_t t13;
    uintptr_t t14;
    uintptr_t t10;
    uint8_t *const *t8;
    uint8_t *t9;
    uint8_t *t7;
    uint32_t t11;
    uint8_t **t15;
    struct Slice2 t16;
    struct Slice2 t2;
    struct MAL t0;
    bool t12;
    t0 = a0;
    t1 = (struct MAL const *)&t0;
    t3 = (struct arr_2_ptr_u8 *)&t2.ptrs;
    t4 = (uintptr_t *)&t2.len;
    t5 = (uintptr_t const *)&t1->len;
    t6 = (*t5);
    (*t4) = t6;
    t4 = (uintptr_t *)&t2.capacity;
    t5 = (uintptr_t const *)&t1->capacity;
    t6 = (*t5);
    (*t4) = t6;
    t8 = (uint8_t *const *)&t1->bytes;
    t9 = (*t8);
    t7 = t9;
    t10 = (uintptr_t)0ul;
zig_loop_22:
    t6 = t10;
    t11 = zig_u32_bitCast_u32(t6, UINT8_C(32));
    t12 = t11 < UINT32_C(2);
    if (t12) {
        t13 = (struct arr_2_usize){{(uintptr_t)4ul, (uintptr_t)1ul}}.array[t6];
        t14 = (struct arr_2_usize){{(uintptr_t)1ul, (uintptr_t)0ul}}.array[t6];
        t3 = (struct arr_2_ptr_u8 *)&t2.ptrs;
        t15 = &t3->array[t14];
        t9 = t7;
        (*t15) = t9;
        t9 = t7;
        t5 = (uintptr_t const *)&t1->capacity;
        t14 = (*t5);
        t14 = t13 * t14;
        t9 = (uint8_t *)(((uintptr_t)t9) + (t14 * sizeof(uint8_t)));
        t7 = t9;
        goto zig_block_1;
    }
    goto zig_block_0;

zig_block_1:;
    t6 = t6 + (uintptr_t)1ul;
    t10 = t6;
    goto zig_loop_22;

zig_block_0:;
    t16 = t2;
    return t16;
}

/* One field: a one-trip loop. */
static struct Slice1 slice1(struct MAL const a0)
{
    struct MAL const *t1;
    uintptr_t t6, t10, t13, t14;
    uint8_t *t7, *t9;
    struct Slice1 t2;
    struct MAL t0;
    t0 = a0;
    t1 = (struct MAL const *)&t0;
    t2.len = t1->len;
    t2.capacity = t1->capacity;
    t9 = t1->bytes;
    t7 = t9;
    t10 = (uintptr_t)0ul;
zig_loop_7:
    t6 = t10;
    if (zig_u32_bitCast_u32(t6, UINT8_C(32)) < UINT32_C(1)) {
        t13 = (struct arr_1_usize){{(uintptr_t)4ul}}.array[t6];
        t14 = (struct arr_1_usize){{(uintptr_t)0ul}}.array[t6];
        t2.ptrs.array[t14] = t7;
        t9 = t7;
        t9 = t9 + t13 * t1->capacity;
        t7 = t9;
        t6 = t6 + (uintptr_t)1ul;
        t10 = t6;
        goto zig_loop_7;
    }
    return t2;
}

/* The same counter read after the loop: stays a loop, and must still count. */
__attribute__((noinline)) static uintptr_t fields_seen(struct MAL const a0, uintptr_t *sum)
{
    uintptr_t t6, t10 = 0, acc = 0;
loop:
    t6 = t10;
    if (zig_u32_bitCast_u32(t6, UINT8_C(32)) < UINT32_C(3)) {
        acc += (struct arr_2_usize){{(uintptr_t)7ul, (uintptr_t)9ul}}.array[t6 & 1] * a0.capacity;
        t6 = t6 + 1;
        t10 = t6;
        goto loop;
    }
    *sum = acc;
    return t6;
}

static struct slice_u8 items_tag(struct MAL m)
{
    struct Slice2 s = slice2(m);
    if (s.capacity == 0)
        return (struct slice_u8){(uint8_t *)0, 0};
    return (struct slice_u8){s.ptrs.array[0], s.len};
}
static struct slice_u32 items_data(struct MAL m)
{
    struct Slice2 s = slice2(m);
    if (s.capacity == 0)
        return (struct slice_u32){(uint32_t *)0, 0};
    return (struct slice_u32){(uint32_t *)s.ptrs.array[1], s.len};
}

struct List { uint8_t *bytes; };
struct Local { struct List items; };

/* itemPtr: an acquire load of the list's bytes, then two slice lookups. */
struct ItemPtr { uint8_t *tag_ptr; uint32_t *data_ptr; };
__attribute__((noinline)) static struct ItemPtr item_ptr(struct Local *const *local, uint32_t index)
{
    uint8_t *bytes = atomic_load_explicit((uint8_t **)&(*local)->items.bytes, memory_order_acquire);
    struct MAL m = {bytes, ((uintptr_t *)bytes)[-1], ((uintptr_t *)bytes)[-1]};
    struct ItemPtr r;
    r.tag_ptr = &items_tag(m).ptr[index];
    r.data_ptr = &items_data(m).ptr[index];
    return r;
}

/* A caller with many inlined slices across a switch: the table addresses are
   shared across the arms. */
__attribute__((noinline)) static uint32_t key_of(struct Local *local, uint32_t index, uint32_t kind)
{
    uint8_t *bytes = __atomic_load_n(&local->items.bytes, __ATOMIC_ACQUIRE);
    struct MAL m = {bytes, ((uintptr_t *)bytes)[-1], ((uintptr_t *)bytes)[-1]};
    switch (kind) {
    case 0: return items_tag(m).ptr[index];
    case 1: return items_data(m).ptr[index];
    case 2: return items_tag(m).ptr[index] * 1000u + items_data(m).ptr[index];
    case 3: {
        struct Slice1 s1 = slice1(m);
        return (uint32_t)(s1.ptrs.array[0] - bytes) + (uint32_t)s1.len;
    }
    case 4: return (uint32_t)items_tag(m).len + (uint32_t)items_data(m).len * 7u;
    default: {
        struct Slice2 s = slice2(m);
        return (uint32_t)(s.ptrs.array[0] - s.ptrs.array[1]);
    }
    }
}

/* A post-increment index in a memory body: the saved old counter is read by
   the store, so each unrolled copy keeps it (990525-1). */
struct pair { int m1, m2; };
__attribute__((noinline)) static int post_inc_store(struct pair arg)
{
    int i;
    struct pair buf[3];
    for (i = 0; i < 3; buf[i++] = arg)
        arg.m1 += i;
    return buf[0].m1 * 100 + buf[1].m1 * 10 + buf[2].m1 + buf[2].m2;
}

/* An unrolled store through `base + (j << 4)`, whose shift was fused into the
   ADD: every copy must scale its constant index (pr93434 stored the imaginary
   parts at t + 17, 18, ... -- unnoticed wherever the stack starts zeroed). */
struct cplx { double re, im; };
__attribute__((noinline)) static int clear_pairs(int fill)
{
    struct cplx t[16];
    unsigned char *p = (unsigned char *)t;
    for (unsigned k = 0; k < sizeof t; k++)
        p[k] = (unsigned char)(fill + k * 7);
    for (int j = 0; j < 16; ++j) {
        t[j].re = 0;
        t[j].im = 0;
    }
    int bad = 0;
    for (unsigned k = 0; k < sizeof t; k++)
        bad += p[k] != 0;
    return bad;
}

static uint32_t loads(uint32_t *p, uint32_t *alias)
{
    uint32_t a = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    *alias = a + 5;
    uint32_t b = __atomic_load_n(p, __ATOMIC_SEQ_CST);
    *alias = b * 3;
    uint32_t c = __atomic_load_n(p, __ATOMIC_RELAXED);
    *alias = c + 1;
    uint32_t d = atomic_load_explicit(p, memory_order_acquire);
    uint16_t h = 0x1234;
    uint16_t e = __atomic_load_n(&h, __ATOMIC_ACQUIRE);
    return a + b * 10 + c * 100 + d * 1000 + e;
}

int main(void)
{
    enum { CAP = 5 };
    static uintptr_t storage[1 + CAP * 2];
    uint8_t *bytes = (uint8_t *)&storage[1];
    storage[0] = CAP;
    uint32_t *data = (uint32_t *)bytes;
    uint8_t *tags = bytes + 4 * CAP;
    for (int i = 0; i < CAP; i++) {
        data[i] = 100 + i * 11;
        tags[i] = (uint8_t)(3 + i);
    }
    struct Local local = {{bytes}};
    struct Local *lp = &local;

    struct MAL m = {bytes, 3, CAP};
    struct Slice2 s = slice2(m);
    printf("slice2 %d %d %u %u\n", (int)(s.ptrs.array[0] - bytes), (int)(s.ptrs.array[1] - bytes),
           (unsigned)s.len, (unsigned)s.capacity);
    struct Slice1 s1 = slice1(m);
    printf("slice1 %d %u %u\n", (int)(s1.ptrs.array[0] - bytes), (unsigned)s1.len, (unsigned)s1.capacity);
    uintptr_t sum = 0;
    uintptr_t seen = fields_seen(m, &sum);
    printf("fields_seen %u %u\n", (unsigned)seen, (unsigned)sum);

    for (uint32_t i = 0; i < CAP; i += 2) {
        struct ItemPtr ip = item_ptr(&lp, i);
        printf("item %u %u %u\n", i, *ip.tag_ptr, *ip.data_ptr);
    }
    uint32_t k = 0;
    for (uint32_t kind = 0; kind < 6; kind++)
        for (uint32_t i = 0; i < CAP; i++)
            k = k * 31 + key_of(&local, i, kind);
    printf("keys %u\n", k);

    printf("post_inc_store %d\n", post_inc_store((struct pair){1, 2}));
    printf("clear_pairs %d %d\n", clear_pairs(1), clear_pairs(0x5a));
    uint32_t x = 7;
    uint32_t l = loads(&x, &x);
    printf("loads %u %u\n", l, x);
    return 0;
}
