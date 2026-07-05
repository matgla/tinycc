/* Exercise ssa_opt.c: address-resolution disambiguation for
 * distinct address-taken locals.  Each pointer must keep its own identity
 * so stores through one do not get forwarded into loads through another. */
#include <stdio.h>

static unsigned mix(unsigned h, unsigned v)
{
    return h * 31u + v;
}

int main(void)
{
    unsigned u1 = 0x11111111, u2 = 0x22222222;
    unsigned u3 = 0x33333333, u4 = 0x44444444;
    unsigned *p1 = &u1, *p2 = &u2, *p3 = &u3, *p4 = &u4;
    *p1 = 1;
    *p2 = 2;
    *p3 = 3;
    *p4 = 4;
    unsigned cs = mix(mix(mix(*p1, *p2), *p3), *p4);
    printf("%08x\n", cs);
    return 0;
}
