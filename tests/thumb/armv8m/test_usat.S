.syntax unified
.thumb

.global _start
_start:

.global test_usat
test_usat:
    usat r0, #1, r1
    usat r2, #16, r3, lsl #1
    usat r10, #20, r11, asr #2

    ite eq
    usateq r0, #1, r1
    usatne r2, #16, r3, lsl #1
