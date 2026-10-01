/* A big constant initializer is lowered to memset(obj, 0, size) followed by
   stores of its non-zero words only.  const_local_table takes the zero fill
   as the start of the table's run, so a compound literal indexed at run time
   -- Zig's fmt.float `Backend64_TablesFull.computePow5`, a 5 KB table with a
   zero low word in its first 27 entries -- is read from .rodata instead of
   being rebuilt on the stack on every call.  Also here: tables past the old
   1 KB limit, and the shapes that must stay on the stack: an element written
   after the initializer, the address passed to a call, and a read of the
   object between the fill and the stores. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

struct pair { uint64_t a[2]; };
struct tbl { struct pair array[40]; };

__attribute__((noinline)) static struct pair get(uint32_t i)
{
    struct pair t1;
    t1 = (struct tbl){{{{0, 1}}, {{0, 2}}, {{5, 3}}, {{0, 4}}, {{0, 5}}, {{7, 6}}, {{0, 7}}, {{0, 8}},
                       {{0, 9}}, {{0, 10}}, {{0, 11}}, {{0, 12}}, {{5, 13}}, {{0, 14}}, {{0, 15}},
                       {{7, 16}}, {{0, 17}}, {{0, 18}}, {{0, 19}}, {{0, 20}}, {{0, 21}}, {{0, 22}},
                       {{5, 23}}, {{0, 24}}, {{0, 25}}, {{7, 26}}, {{0, 27}}, {{0, 28}}, {{0, 29}},
                       {{0, 30}}, {{0, 31}}, {{0, 32}}, {{5, 33}}, {{0, 34}}, {{0, 35}},
                       {{7, 36}}, {{0, 37}}, {{0, 38}}, {{0, 39}}, {{0x123456789abcdefull, 40}}}}
             .array[i];
    return t1;
}

/* 2 KB, mostly zeros: past the old 1 KB cap. */
struct big { uint32_t array[512]; };
__attribute__((noinline)) static uint32_t big_at(uint32_t i)
{
    return (struct big){{[3] = 33, [100] = 1000, [511] = 0xdeadbeef}}.array[i];
}

/* Written after the initializer: the stack copy is the table. */
__attribute__((noinline)) static uint32_t rewritten(uint32_t i, uint32_t v)
{
    struct big t = {{[3] = 33, [100] = 1000, [511] = 7}};
    t.array[i] = v;
    return t.array[3] + t.array[100] + t.array[511];
}

__attribute__((noinline)) static uint32_t sum_words(const uint32_t *p, int n)
{
    uint32_t s = 0;
    for (int k = 0; k < n; k++)
        s += p[k];
    return s;
}

/* The address reaches a call: kept. */
__attribute__((noinline)) static uint32_t escapes(uint32_t i)
{
    struct big t = {{[3] = 33, [100] = 1000, [511] = 9}};
    return sum_words(t.array, 512) + t.array[i];
}

int main(void)
{
    uint64_t lo = 0, hi = 0;
    for (uint32_t i = 0; i < 40; i++)
    {
        struct pair p = get(i);
        lo = lo * 31 + p.a[0];
        hi = hi * 31 + p.a[1];
    }
    printf("pairs %llu %llu\n", (unsigned long long)lo, (unsigned long long)hi);
    printf("big %u %u %u %u\n", (unsigned)big_at(0), (unsigned)big_at(3), (unsigned)big_at(100),
           (unsigned)big_at(511));
    printf("rewritten %u %u\n", (unsigned)rewritten(3, 5), (unsigned)rewritten(200, 5));
    printf("escapes %u %u\n", (unsigned)escapes(3), (unsigned)escapes(4));
    return 0;
}
