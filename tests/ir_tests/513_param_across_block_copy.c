/* A leaf whose body opens with a 64-byte-or-larger BLOCK_COPY -- a local
   array initialised from a .rodata template -- runs that copy in r0-r3/IP/LR,
   as it would a memcpy call.  The allocator's call-crossing test counted only
   calls strictly after an interval's start, and a parameter's interval starts
   at instruction 0, where the copy sits: `i` stayed precolored in r0 and was
   overwritten by the copy's destination address.  One, two and four register
   parameters, and a 64-bit one. */
#include <stdio.h>
#include <stdint.h>

__attribute__((noinline)) static uint32_t one(int i)
{
    const uint32_t t[24] = {3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3, 2, 3, 8, 4, 6, 2, 6, 4};
    return t[i % 24] * 100 + t[(i + 7) % 24];
}

__attribute__((noinline)) static uint32_t two(uint32_t a, uint32_t b)
{
    const uint32_t t[16] = {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26};
    return t[a & 15] * b + t[b & 15];
}

__attribute__((noinline)) static uint32_t four(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    const uint32_t t[20] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110};
    return t[a % 20] + t[b % 20] * 3 + t[c % 20] * 5 + t[d % 20] * 7 + a + b + c + d;
}

__attribute__((noinline)) static uint64_t wide(uint64_t x)
{
    const uint64_t t[8] = {1, 3, 5, 7, 11, 13, 17, 19};
    return t[x & 7] * x + t[(x >> 32) & 7];
}

int main(void)
{
    printf("one %u %u %u\n", one(0), one(11), one(23));
    printf("two %u %u\n", two(3, 5), two(17, 2));
    printf("four %u\n", four(1, 22, 45, 7));
    printf("wide %llu\n", (unsigned long long)wide(0x500000003ull));
    return 0;
}
