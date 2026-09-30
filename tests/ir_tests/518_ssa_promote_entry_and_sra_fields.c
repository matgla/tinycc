/* SSA promotion of VARs the old rules left in memory, and the gates that keep
   the widening sound.  Promoted now: a VAR defined only in the entry block
   (which nothing jumps back to), read in later blocks and in a loop; and the
   field VARs SRA makes for a small struct, whose defs are all slot stores.
   Kept as before: a VAR whose only def is NOT in the entry block (an empty
   dominance frontier is not trusted: the un-rotated loop shape under-reports
   it, gcc-torture pr78675), and a VAR read at two offsets (renaming clears the
   offset: a _Complex double's two halves, ieee/cdivchk). */
#include <stdio.h>
#include <stdint.h>

struct pair { uint32_t a, b; };
struct eu { uint32_t payload; uint16_t error; };

__attribute__((noinline)) static int entry_only(int a, int n)
{
    int k = a * 3;
    int base = a - 1;
    int s = 0;
    for (int i = 0; i < n; i++)
        s += k + i;
    if (s > 100)
        s -= base;
    return s + k;
}

__attribute__((noinline)) static int def_in_loop(int n)
{
    int last;
    int s = 0;
    for (int i = 0; i < n; i++)
    {
        if (i == n - 1)
            last = i * 7;
        s += i;
    }
    return n > 0 ? s + last : s;
}

__attribute__((noinline)) static double halves(double _Complex z)
{
    double re = __real__ z;
    double im = __imag__ z;
    return re * 10 + im;
}

__attribute__((noinline)) static uint32_t sra_fields(uint32_t x, uint32_t y, int n)
{
    struct pair t;
    t.a = x;
    t.b = y;
    for (int i = 0; i < n; i++)
    {
        uint32_t na = t.b + (uint32_t)i;
        t.b = t.a ^ na;
        t.a = na;
    }
    return t.a * 3 + t.b;
}

__attribute__((noinline)) static struct eu make_eu(uint32_t v, int fail)
{
    struct eu r;
    if (fail)
    {
        r.payload = 0xaaaaaaaa;
        r.error = 5;
    }
    else
    {
        r.payload = v * 2;
        r.error = 0;
    }
    return r;
}

__attribute__((noinline)) static uint32_t use_eu(uint32_t v)
{
    struct eu t = make_eu(v, v > 50);
    struct eu u = t;
    if (u.error != 0)
        return 1000u + u.error;
    return u.payload;
}

/* The Zig C backend's shapes (from zig.c): a parameter copied into a local and
   read back through a pointer local, the same field loaded twice through it
   (GenZir.isEmpty) -- promoted, the second load is the first one's value. */
struct list { uint32_t *items; uintptr_t len; };
struct genzir { uint32_t pad[4]; struct list *instructions; uintptr_t instructions_top; };

__attribute__((noinline)) static _Bool zig_is_empty(struct genzir const *const a0)
{
    struct genzir const *const *t1;
    struct genzir const *t2;
    struct genzir const *t0;
    uintptr_t const *t3;
    uintptr_t t4, t12;
    struct list *const *t8;
    struct list *t9;
    uintptr_t *t11;
    _Bool t6, t7;
    t0 = a0;
    t1 = (struct genzir const *const *)&t0;
    t2 = (*t1);
    t3 = (uintptr_t const *)&t2->instructions_top;
    t4 = (*t3);
    t6 = t4 == (uintptr_t)-1;
    if (t6) {
        t7 = 1;
        goto zig_block_0;
    }
    t2 = (*t1);
    t8 = (struct list *const *)&t2->instructions;
    t9 = (*t8);
    t11 = &t9->len;
    t4 = (*t11);
    t2 = (*t1);
    t3 = (uintptr_t const *)&t2->instructions_top;
    t12 = (*t3);
    t6 = t4 == t12;
    t7 = t6;
    goto zig_block_0;
zig_block_0:;
    return t7;
}

/* An optional built in one local and copied into the returned one
   (Ast.Node.OptionalIndex.unwrap): SRA's field VARs, promoted. */
struct opt_index { uint32_t payload; _Bool is_null; };

__attribute__((noinline)) static struct opt_index zig_unwrap(uint32_t const a0)
{
    struct opt_index t0;
    struct opt_index t4;
    uint32_t t2;
    _Bool t1;
    t1 = a0 == UINT32_MAX;
    if (t1) {
        t0 = (struct opt_index){ .is_null = 1, .payload = UINT32_C(0xaaaaaaaa) };
        goto zig_block_0;
    }
    t2 = a0 + 1;
    t4.is_null = 0;
    t4.payload = t2;
    t0 = t4;
    goto zig_block_0;
zig_block_0:;
    return t0;
}

int main(void)
{
    uint32_t items[3] = {1, 2, 3};
    struct list l = {items, 3};
    struct genzir g1 = {{0}, &l, 3}, g2 = {{0}, &l, 1}, g3 = {{0}, &l, (uintptr_t)-1};
    printf("empty %d %d %d\n", zig_is_empty(&g1), zig_is_empty(&g2), zig_is_empty(&g3));
    struct opt_index o1 = zig_unwrap(41), o2 = zig_unwrap(UINT32_MAX);
    printf("unwrap %u %d %d\n", (unsigned)o1.payload, o1.is_null, o2.is_null);
    printf("entry %d %d %d\n", entry_only(4, 3), entry_only(9, 20), entry_only(-2, 0));
    printf("loop %d %d\n", def_in_loop(5), def_in_loop(0));
    printf("halves %.1f %.1f\n", halves(2.0 + 3.0 * 1.0i), halves(-1.5 + 0.5 * 1.0i));
    printf("sra %u %u\n", (unsigned)sra_fields(1, 2, 5), (unsigned)sra_fields(7, 3, 0));
    printf("eu %u %u\n", (unsigned)use_eu(21), (unsigned)use_eu(60));
    return 0;
}
