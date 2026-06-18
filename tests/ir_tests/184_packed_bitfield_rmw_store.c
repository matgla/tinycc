/* Regression: packed >32-bit bitfield RMW store at -O1/-O2.
 *
 * The three packed RMW field updates (`s->a += 5`, `++s->a`, `s->b = 2`) each
 * materialise the same `s + 10` address temp.  When the first update's read-back
 * LOAD is store-to-load-forwarded via the stack-store-forward path in
 * ssa_opt_load_cse, that path used to forget to drop the LOAD's use of its base
 * pointer.  The stale use entry corrupted the base's use-list so a later
 * copy-prop failed to rewrite the *store's* address operand, leaving it pointing
 * at an undefined spill slot — the first store then wrote `s->a` through a
 * garbage address (observed as 95_bitfields TEST2 PACKED HardFaulting at -O1).
 *
 * Byte dump after the updates must equal 95_bitfields' "TEST 2 - PACKED"
 * golden value 4A48D159E26AF37BC1E003 (high byte first); a corrupted `s->a`
 * store changes the trailing bytes (or faults).
 */
#include <stdio.h>
#include <string.h>

#pragma pack(push, 1)
struct __s {
    int x : 12;
    char y : 6;
    long long z : 63;
    char a : 4;
    long long b : 2;
};
#pragma pack(pop)

static void dump(void *p, int n)
{
    int i;
    for (i = n; --i >= 0;)
        printf("%02X", ((unsigned char *)p)[i]);
    printf("\n");
}

int main(void)
{
    struct __s _s, *s = &_s;
    memset(s, 0, sizeof *s);
    s->x = -1, s->y = -1, s->z = -1, s->a = -1, s->b = -1;
    s->x = 3, s->y = 30, s->z = 0x123456789abcdef0LL, s->a += 5, ++s->a, s->b = 2;
    dump(s, sizeof *s);
    return 0;
}
