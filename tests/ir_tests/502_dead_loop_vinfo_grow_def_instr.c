/* ssa:dead_loop grew the SSA vreg table for the TEMPs it creates with 16
   spare entries, zero-filled -- and a zero entry reads as "defined by
   instruction 0, never used".  The next ssa:dce deleted instruction 0 on the
   spare entry's behalf: here the entry block's `i = -5`, so main's first loop
   ran from whatever r4 held and printed nothing.  It takes a loop dead_loop
   rewrites (in_loop(0), inlined, its switch folded to a constant) after one
   whose counter is set in the entry block. */
#include <stdio.h>
static int variable(int i)
{
    switch (i) {
    case 0: return 70;
    case 1: return 71;
    case 2: return 72;
    case 3: return 73;
    case 4: return 74;
    default: return -70;
    }
}
static int in_loop(int iterations)
{
    int r = 0;
    for (int n = 0; n < iterations; n++) {
        int i = 7;
        r = 1000;
        switch (i) {
        case 0: r += i + 1; break;
        case 1: r -= i; break;
        case 2: r += 1; break;
        case 3: r = r / 2 + 1; break;
        case 4: r ^= i; break;
        case 5: r &= (0xffff + i); break;
        case 6: r |= (i & 0x0f); break;
        case 7: r = (r ^ 0xff) ^ 0xff; break;
        default: r = 0; break;
        }
    }
    return r;
}
int main(void)
{
    for (int i = -5; i <= 9; i++)
        printf("%d\n", variable(i));
    printf("%d\n", in_loop(0));
    return 0;
}
