.syntax unified
.thumb

.global _start
_start:

.global test_sel
test_sel:
    sel r0, r1, r2
    sel r3, r4, r5
    sel r7, r7, r0

    ite eq
    seleq r0, r1, r2
    selne r3, r4, r5

    sel r9, r10, r11
    sel r12, r14, r0