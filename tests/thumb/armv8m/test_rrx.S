.syntax unified
.thumb

.global _start
_start:

.global test_rrx
test_rrx:
    rrx r0, r1
    rrx.w r0, r1
    rrx r11, r2

    ite eq
    rrxeq r2, r1
    rrxne r11, r8

    ite gt
    rrxgt r1, r2
    rrxle r2, r1

    rrxs r0, r1
    rrxs.w r0, r1
    rrxs r11, r2
