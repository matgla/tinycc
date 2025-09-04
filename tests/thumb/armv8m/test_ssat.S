.syntax unified
.thumb

.global _start
_start:

.global test_ssat
test_ssat:
    ssat r0, #1, r1
    ssat r2, #16, r3, lsl #1
    ssat r10, #20, r11, asr #2

    ite eq
    ssateq r0, #1, r1
    ssatne r2, #16, r3, lsl #1
