.syntax unified
.thumb

.global _start
_start:

.global test_usub8
test_usub8:
    usub8 r0, r1, r2
    usub8 r3, r4, r5
    usub8 r7, r7, r0

    ite eq
    usub8eq r0, r1, r2
    usub8ne r3, r4, r5

    usub8 r9, r10, r11
    usub8 r12, r14, r0