.syntax unified
.thumb

.global _start
_start:

.global test_umlal
test_umlal:
    umlal r0, r1, r2, r3
    umlal r8, r9, r10, r11

    ite eq
    umlaleq r0, r1, r2, r3
    umlalne r8, r9, r10, r11