.syntax unified
.thumb

.global _start
_start:

.global test_smlal
test_smlal:
    smlal r0, r1, r2, r3
    smlal r8, r9, r10, r11

    ite eq
    smlaleq r0, r1, r2, r3
    smlalne r8, r9, r10, r11