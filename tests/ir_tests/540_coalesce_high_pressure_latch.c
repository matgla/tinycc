/* The graph coalescer used to give up on a whole function once any point in
   it had as many live values as registers.  It now still merges a copy class
   whose members' ranges leave no gap -- a loop's `i - 1` and `acc << 8 | b`
   latch copies lie inside the phi's range, so the merge adds pressure
   nowhere.  Zig's readInt inside Wyhash.hash, next to a u128 multiply, is the
   shape; this checks the loops still compute the same values next to 64-bit
   products that fill the registers, with narrow values spilled around them. */
#include <stdio.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

__attribute__((noinline)) static u32 read_then_mix(const u8 *p, u32 n, u64 s0, u64 s1, u64 s2, u64 s3)
{
    u32 acc = 0;
    u32 i = n;
    for (;;)
    {
        acc = acc << 8 | p[i];
        if (i == 0)
            break;
        i = i - 1;
    }
    u64 a = s0 * s1, b = s1 * s2, c = s2 * s3, d = s3 * s0;
    u64 e = a ^ c, f = b ^ d, g = a + d, h = b + c;
    return (u32)(a ^ b ^ c ^ d ^ (e * f) ^ (g * h) ^ (e + h) ^ (f + g)) + acc;
}

__attribute__((noinline)) static u32 bytes_then_select(const u8 *p, u32 n, u8 mode, u64 s0, u64 s1)
{
    u32 acc = 0, sum = 0;
    for (u32 i = 0; i < n; i++)
    {
        u32 b = p[i];
        acc = acc * 31 + b;
        sum = sum + (b ^ mode);
    }
    u64 a = s0 * s1, c = s0 + s1, d = s0 ^ (s1 << 7);
    u64 x = a * c ^ d * a ^ c * d;
    switch (mode)
    {
    case 0: return (u32)x + acc;
    case 1: return (u32)(x >> 32) ^ sum;
    default: return acc - sum + (u32)x;
    }
}

int main(void)
{
    static const u8 bytes[16] = {0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x90,
                                 0x01, 0x80, 0x7f, 0xff, 0x10, 0x20, 0x30, 0x40};
    printf("%08x\n", (unsigned)read_then_mix(bytes, 3, 0x123456789ull, 0xfedcba987ull, 7, 0x1111111111ull));
    printf("%08x\n", (unsigned)read_then_mix(bytes + 4, 3, 1, 2, 3, 4));
    printf("%08x\n", (unsigned)read_then_mix(bytes + 8, 0, 5, 6, 7, 8));
    for (u8 m = 0; m < 3; m++)
        printf("%u %08x\n", m, (unsigned)bytes_then_select(bytes, 16, m, 0xdeadbeefcafeull, 0x1234567ull));
    return 0;
}
