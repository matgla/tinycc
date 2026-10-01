/* An inlined helper's result reaches its caller through `V3 <- V2`, which
   ssa_rename spells `T14 <-- T16 [STORE]`: a copy between two TEMPs.  cprop
   forwards it like an ASSIGN copy (it was left for the allocator, as a
   register move in every loop turn of Zig's readInt).  The copy must still
   keep a truncation when the two sides differ in width, and a TEMP stored
   twice is not a copy. */
#include <stdio.h>
#include <stdint.h>

static inline uint32_t cast8(uint8_t v) { return v; }
static inline uint8_t low8(uint32_t v) { return (uint8_t)v; }
static inline int16_t low16s(uint32_t v) { return (int16_t)v; }

struct arr_4_u8 { uint8_t array[4]; };

/* Zig's std.mem.readInt(u32, .little), as the C backend emits it. */
__attribute__((noinline)) static uint32_t read_le32(const uint8_t *p)
{
    struct arr_4_u8 t21;
    uintptr_t t17, t23;
    uint32_t t24, t26, t27, t22;
    uint8_t t25;
    t21 = *(const struct arr_4_u8 *)p;
    t23 = 3;
    t24 = 0;
zig_loop:
    t17 = t23;
    t25 = t21.array[t17];
    t26 = cast8(t25);
    t27 = t24;
    t27 = t27 << 8;
    t26 = t27 | t26;
    if (t17 == 0)
    {
        t22 = t26;
        goto out;
    }
    t24 = t26;
    t17 = t17 - 1;
    t23 = t17;
    goto zig_loop;
out:
    return t22;
}

__attribute__((noinline)) static uint32_t narrow_sum(const uint32_t *p, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i++)
    {
        uint8_t b = low8(p[i]);
        int16_t h = low16s(p[i]);
        s = s * 3 + b + (uint32_t)(int32_t)h;
    }
    return s;
}

__attribute__((noinline)) static uint32_t twice(uint32_t a, uint32_t b)
{
    uint32_t x = cast8((uint8_t)a), y;
    y = x;
    x = cast8((uint8_t)b);
    return y * 1000 + x;
}

int main(void)
{
    static const uint8_t bytes[8] = {0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x90};
    static const uint32_t words[4] = {0x12348081u, 0xffff7fffu, 0x000000ffu, 0xabcdef01u};
    printf("le32 %08x %08x\n", (unsigned)read_le32(bytes), (unsigned)read_le32(bytes + 4));
    printf("narrow %u\n", (unsigned)narrow_sum(words, 4));
    printf("twice %u\n", (unsigned)twice(0x1ff, 0x2ab));
    return 0;
}
