/* A 64-bit or narrow slot STORE that covers every read of a local is a fresh
   SSA definition (a narrow one only when it stores a constant, written as the
   local holds it), and a local only ever stored one constant reads as that
   constant everywhere (sra.c, sra_forward_const_vars).  The inliner binds a
   parameter with a slot STORE: the Zig C backend's zig.h passes `bits` (a
   uint8_t) and 64-bit halves to every u128 helper this way. */
#include <stdio.h>
#include <stdint.h>

static inline uint64_t sign_trunc(uint64_t v, uint8_t bits)
{
    if (bits >= 64)
        return (int64_t)v < 0 ? v | 0x8000000000000000ull : v & 0x7fffffffffffffffull;
    uint64_t m = (1ull << bits) - 1;
    return (v & (1ull << (bits - 1))) ? v | ~m : v & m;
}

static inline uint64_t shr_by(uint64_t hi, uint64_t lo, uint8_t n)
{
    if (n == 0) return lo;
    if (n >= 64) return hi >> (n - 64);
    return hi << (64 - n) | lo >> n;
}

__attribute__((noinline)) static uint64_t helpers(uint64_t a, int c)
{
    uint64_t r = sign_trunc(a, 64) ^ sign_trunc(a, 12);
    if (c)
        r += shr_by(0, a, 64);
    else
        r += shr_by(a, a ^ 1, 3);
    return r + shr_by(a, 7, 0);
}

/* Narrow constants: the store truncates, the value read must too. */
__attribute__((noinline)) static int narrow(int c)
{
    unsigned char u;
    signed char s;
    unsigned short w;
    u = 300;
    s = 200;
    w = 70000;
    int r = 0;
    if (c > 2)
        r = u;
    else if (c > 1)
        r = s;
    else
        r = w;
    return r + (c & 1 ? u : s);
}

/* Written twice with the same constant: still that constant. */
__attribute__((noinline)) static int same_twice(int c)
{
    int k;
    k = 41;
    if (c)
        k = 41;
    return k + c;
}

/* Two different constants: never folded. */
__attribute__((noinline)) static int two_values(int c)
{
    int k;
    k = 5;
    if (c & 1)
        k = 9;
    return k * 3 + c;
}

/* Read before the write in a loop: every read after the first iteration sees
   the constant; the first reads a value the program sets itself. */
__attribute__((noinline)) static int loop_read_first(int n)
{
    int acc = 0, k = 0;
    for (int i = 0; i < n; i++) {
        acc += k;
        k = 17;
    }
    return acc;
}

int main(void)
{
    uint64_t sum = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t a = 0x0123456789abcdefull * (uint64_t)(i + 1) ^ ((uint64_t)i << 60);
        sum = sum * 31 + helpers(a, i & 1);
    }
    printf("%016llx\n", (unsigned long long)sum);
    for (int c = 0; c < 4; c++)
        printf("narrow %d %d\n", c, narrow(c));
    printf("same %d %d\n", same_twice(0), same_twice(3));
    printf("two %d %d\n", two_values(0), two_values(1));
    printf("loop %d %d\n", loop_read_first(1), loop_read_first(5));
    return 0;
}
