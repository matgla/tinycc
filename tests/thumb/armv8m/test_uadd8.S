.syntax unified
.thumb

.global _start
_start:

.global test_uadd8
test_uadd8:
    uadd8 r0, r1, r2
    uadd8 r3, r4, r5
    uadd8 r7, r7, r0

    ite eq
    uadd8eq r0, r1, r2
    uadd8ne r3, r4, r5

    uadd8 r9, r10, r11
    uadd8 r12, r14, r0