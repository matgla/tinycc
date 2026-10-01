/* A struct assignment the frontend lowers to __aeabi_memmove4/8 with a constant
   size of up to 128 bytes is emitted as the copy itself (ldmia/stmia through
   r0/r1 with r2, r3, r12 and lr) instead of a call.  A copy that reaches either
   side through a packed member names plain __aeabi_memmove: its address need
   not be word aligned, and LDM/STM fault on one that is not. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define DEFS(N) typedef struct { uint32_t w[N]; } s##N;
DEFS(1) DEFS(2) DEFS(3) DEFS(5) DEFS(8) DEFS(13) DEFS(24) DEFS(32) DEFS(33)
typedef struct { uint64_t d[5]; double f; } s48d; /* 8-aligned: memmove8 */

static uint32_t mix;
#define CHECK(N)                                                               \
    __attribute__((noinline)) static uint32_t copy##N(const s##N *p, s##N *q)  \
    {                                                                          \
        s##N t = *p;                                                           \
        t.w[N - 1] ^= 0x5a5a5a5au;                                             \
        *q = t;                                                                \
        uint32_t h = 0;                                                        \
        for (int i = 0; i < N; i++)                                            \
            h = h * 33 + q->w[i];                                              \
        return h;                                                              \
    }
CHECK(1) CHECK(2) CHECK(3) CHECK(5) CHECK(8) CHECK(13) CHECK(24) CHECK(32) CHECK(33)

__attribute__((noinline)) static double copy48d(const s48d *p, s48d *q)
{
    s48d t = *p;
    t.f += 0.5;
    *q = t;
    return q->f + (double)(q->d[4] & 0xffff);
}

/* Exact self-assignment through two pointers to the same object. */
__attribute__((noinline)) static void self_copy(s13 *a, s13 *b) { *a = *b; }

/* Packed members at odd offsets: the copies must not assume alignment. */
struct __attribute__((packed)) pk {
    char c;
    s5 inner;
    char d;
    s8 inner2;
};
__attribute__((noinline)) static uint32_t packed_copy(struct pk *x, const s5 *v, const s8 *v2)
{
    x->inner = *v;
    x->inner2 = *v2;
    s5 back = x->inner;
    s8 back2 = x->inner2;
    return back.w[0] + back.w[4] + back2.w[7] + (uint32_t)x->c + (uint32_t)x->d;
}

/* An array of structs walked by index: the copies are in a loop. */
__attribute__((noinline)) static uint32_t rotate(s24 *arr, int n)
{
    s24 first = arr[0];
    for (int i = 0; i + 1 < n; i++)
        arr[i] = arr[i + 1];
    arr[n - 1] = first;
    uint32_t h = 0;
    for (int i = 0; i < n; i++)
        h = h * 7 + arr[i].w[i % 24];
    return h;
}

#define RUN(N)                                                                 \
    {                                                                          \
        s##N a, b;                                                             \
        for (int i = 0; i < N; i++)                                            \
            a.w[i] = 0x01010101u * (uint32_t)(i + N);                          \
        memset(&b, 0, sizeof b);                                               \
        uint32_t h = copy##N(&a, &b);                                          \
        printf("s%d %08x %08x\n", N, h, b.w[N - 1]);                           \
    }

int main(void)
{
    RUN(1) RUN(2) RUN(3) RUN(5) RUN(8) RUN(13) RUN(24) RUN(32) RUN(33)
    s48d x, y;
    for (int i = 0; i < 5; i++)
        x.d[i] = 0x0102030405060708ull * (uint64_t)(i + 1);
    x.f = 3.25;
    double r48 = copy48d(&x, &y);
    printf("s48d %.2f %016llx\n", r48, (unsigned long long)y.d[2]);
    s13 z;
    for (int i = 0; i < 13; i++)
        z.w[i] = (uint32_t)i * 3u;
    self_copy(&z, &z);
    printf("self %u %u\n", z.w[0], z.w[12]);
    struct pk k;
    memset(&k, 0, sizeof k);
    k.c = 7;
    k.d = 9;
    s5 v = {{10, 20, 30, 40, 50}};
    s8 v2 = {{1, 2, 3, 4, 5, 6, 7, 8}};
    printf("packed %u\n", packed_copy(&k, &v, &v2));
    s24 arr[5];
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 24; j++)
            arr[i].w[j] = (uint32_t)(i * 100 + j);
    uint32_t rh = rotate(arr, 5);
    printf("rotate %08x %u\n", rh, arr[4].w[0]);
    return 0;
}
