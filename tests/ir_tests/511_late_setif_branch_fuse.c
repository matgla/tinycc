/* The Zig C backend declares one `bool t` per function and reuses it for
   every check, copying it around between them.  The flat setif_fuse runs
   before SSA, when `t` is still one variable with several non-test reads, so
   it keeps every SETIF.  SSA renaming then gives each check its own
   single-use temp, and `CMP; t <- SETIF; TEST_ZERO t; JUMPIF` fuses into
   `CMP; JUMPIF` once more after phi resolution (ra:setif_fuse).  Every
   relation, signed and unsigned, tested both ways round (`if (t)` and
   `if (!t)`), so each fused condition -- including the inverted ones -- is
   checked against the value the SETIF would have produced. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

__attribute__((noinline)) static unsigned classify(int32_t a, int32_t b, uint32_t ua, uint32_t ub)
{
    bool t4, t19;
    unsigned r = 0;
    t4 = a < b;   if (t4) r |= 1u << 0;
    t4 = a <= b;  if (!t4) r |= 1u << 1;
    t4 = a > b;   t19 = t4; if (t19) r |= 1u << 2;
    t4 = a >= b;  if (t4) goto l3; r |= 1u << 3;
l3:
    t4 = a == b;  if (!t4) r |= 1u << 4;
    t4 = a != b;  if (t4) r |= 1u << 5;
    t4 = ua < ub; if (t4) r |= 1u << 6;
    t4 = ua <= ub; t19 = t4; if (!t19) r |= 1u << 7;
    t4 = ua > ub; if (t4) r |= 1u << 8;
    t4 = ua >= ub; if (!t4) r |= 1u << 9;
    t4 = (ua & 0x80u) != 0; if (t4) r |= 1u << 10;
    t4 = (uint8_t)ua == 0;  if (!t4) r |= 1u << 11;
    return r;
}

/* The probe-loop shape: `t19` joins a SETIF and a constant false. */
__attribute__((noinline)) static int probe(const uint8_t *meta, uint32_t n, uint32_t limit)
{
    bool t4, t19;
    uint32_t i = 0;
    for (;;) {
        t4 = !(meta[i] == 0);
        if (t4) {
            t4 = limit != 0;
            t19 = t4;
            goto b3;
        }
        t19 = false;
b3:
        if (!t19)
            return (int)i;
        limit--;
        i = (i + 1) % n;
    }
}

int main(void)
{
    static const int32_t v[] = {-5, -1, 0, 1, 7, INT32_MIN, INT32_MAX};
    unsigned acc = 0;
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 7; j++)
            acc = acc * 31 + classify(v[i], v[j], (uint32_t)v[i], (uint32_t)v[j]);
    printf("classify %08x\n", acc);
    printf("one %03x %03x %03x\n", classify(1, 2, 1, 2), classify(2, 1, 0x80, 1), classify(3, 3, 0x100, 0x100));
    static const uint8_t meta[8] = {3, 5, 0x81, 9, 0, 7, 1, 2};
    printf("probe %d %d %d\n", probe(meta, 8, 100), probe(meta, 8, 2), probe(meta + 5, 3, 100));
    return 0;
}
