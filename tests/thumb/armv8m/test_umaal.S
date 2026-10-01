.syntax unified
.thumb

.global _start
_start:

.global test_umaal
test_umaal:
    umaal r0, r1, r2, r3
    umaal r8, r9, r10, r11
    umaal r3, r12, lr, r4

    ite eq
    umaaleq r0, r1, r2, r3
    umaalne r8, r9, r10, r11
