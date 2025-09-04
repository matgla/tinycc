.syntax unified
.thumb

.global _start
_start:

.global test_umull
test_umull:
    umull r0, r1, r2, r3
    umull r8, r9, r10, r11

    ite eq
    umulleq r0, r1, r2, r3
    umullne r8, r9, r10, r11