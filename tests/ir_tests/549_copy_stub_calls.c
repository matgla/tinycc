/* Word-aligned block copies of 3..32 words call libtcc1's __tcc_wcopy_N
   (lib/arm_copystub.S) instead of expanding inline: struct assignment
   (__aeabi_memmove4/8), a plain memcpy the compiler proves aligned, a struct
   argument copied into the outgoing area (also for a tail call, whose copy
   must still return), and an initialiser image copied into a leaf function's
   local (the call clobbers LR, which the leaf saves around it).  Every size
   from 3 to 33 words, so each entry into the three chains is run, and the
   words around the destination stay untouched. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define GUARD 0xdeadbeefu
#define SIZES(X)                                                                \
    X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)       \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27)      \
    X(28) X(29) X(30) X(31) X(32) X(33)

#define DEFS(N)                                                                  \
    typedef struct { uint32_t w[N]; } s##N;                                      \
    typedef struct { uint32_t lo; s##N v; uint32_t hi; } g##N;
SIZES(DEFS)
typedef struct { uint64_t d[7]; } d7; /* 8-aligned: __aeabi_memmove8 */

static uint32_t hash(const uint32_t *w, int n)
{
    uint32_t h = 0;
    for (int i = 0; i < n; i++)
        h = h * 33 + w[i];
    return h;
}

static uint32_t fail;
static void check_guard(const char *what, int n, uint32_t lo, uint32_t hi)
{
    if (lo != GUARD || hi != GUARD)
    {
        printf("%s %d: guard %08x %08x\n", what, n, (unsigned)lo, (unsigned)hi);
        fail++;
    }
}

/* Struct assignment into the middle of a guarded object. */
#define ASSIGN(N)                                                                \
    __attribute__((noinline)) static uint32_t assign##N(const s##N *p)           \
    {                                                                            \
        g##N g;                                                                  \
        g.lo = g.hi = GUARD;                                                     \
        g.v = *p;                                                                \
        check_guard("assign", N, g.lo, g.hi);                                    \
        return hash(g.v.w, N);                                                   \
    }
SIZES(ASSIGN)

/* A plain memcpy between word-aligned locals. */
#define MEMCPY(N)                                                                \
    __attribute__((noinline)) static uint32_t mcpy##N(const s##N *p)             \
    {                                                                            \
        s##N a = *p, b;                                                          \
        a.w[0] ^= 1;                                                             \
        memcpy(&b, &a, sizeof b);                                                \
        return hash(b.w, N);                                                     \
    }
SIZES(MEMCPY)

/* By value: past r0-r3 the struct is copied into the outgoing area. */
#define BYVAL(N)                                                                 \
    __attribute__((noinline)) static uint32_t take##N(int a, int b, s##N v)      \
    {                                                                            \
        return hash(v.w, N) + (uint32_t)(a * 7 + b);                             \
    }                                                                            \
    __attribute__((noinline)) static uint32_t pass##N(const s##N *p)             \
    {                                                                            \
        s##N v = *p;                                                             \
        v.w[N - 1] += 3;                                                         \
        uint32_t r = take##N(1, 2, v);                                           \
        return r + take##N(3, 4, v);                                             \
    }                                                                            \
    __attribute__((noinline)) static uint32_t tail##N(const s##N *p)             \
    {                                                                            \
        return take##N(5, 6, *p);                                                \
    }
SIZES(BYVAL)

/* An initialiser image copied into a local of a leaf function. */
#define INIT(N)                                                                  \
    __attribute__((noinline)) static uint32_t init##N(uint32_t k)                \
    {                                                                            \
        s##N v = {{[0] = 0x11, [1] = 0x2200, [2] = 0x330000, [N / 2] = 0x44,     \
                   [N - 1] = 0x55000000}};                                       \
        v.w[k % N] += k;                                                         \
        uint32_t h = 0;                                                          \
        for (int i = 0; i < N; i++)                                              \
            h = h * 33 + v.w[i];                                                 \
        return h;                                                                \
    }
SIZES(INIT)

__attribute__((noinline)) static uint64_t copy_d7(const d7 *p)
{
    d7 t = *p;
    t.d[6] ^= 0x0123456789abcdefull;
    d7 u = t;
    uint64_t h = 0;
    for (int i = 0; i < 7; i++)
        h = h * 31 + u.d[i];
    return h;
}

int main(void)
{
    uint32_t src[33];
    for (int i = 0; i < 33; i++)
        src[i] = 0x10203040u * (uint32_t)(i + 1) + (uint32_t)i;
    uint32_t total = 0;
#define RUN(N)                                                                   \
    {                                                                            \
        s##N s;                                                                  \
        memcpy(&s, src, sizeof s);                                               \
        uint32_t a = assign##N(&s), m = mcpy##N(&s), b = pass##N(&s),            \
                 t = tail##N(&s), c = init##N(N + 5);                            \
        printf("%2d %08x %08x %08x %08x %08x\n", N, (unsigned)a, (unsigned)m,    \
               (unsigned)b, (unsigned)t, (unsigned)c);                           \
        total += a ^ m ^ b ^ t ^ c;                                              \
    }
    SIZES(RUN)
    d7 d;
    for (int i = 0; i < 7; i++)
        d.d[i] = 0x1111111111111111ull * (uint64_t)(i + 1);
    uint64_t h = copy_d7(&d);
    printf("d7 %08x%08x\n", (unsigned)(h >> 32), (unsigned)h);
    printf("total %08x fail %u\n", (unsigned)total, (unsigned)fail);
    return 0;
}
