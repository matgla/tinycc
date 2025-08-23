.syntax unified
.thumb

.global _start
_start:

.global test_pkhbt
test_pkhbt:
    pkhbt r0, r1, r2, lsl #8
    pkhbt r3, r4, r5, lsl #16
    pkhbt r9, r10, r11

    ite eq
    pkhbteq r0, r1, r2, lsl #8
    pkhbtne r3, r4, r5, lsl #16

    pkhtb r0, r1, r2, asr #8
    pkhtb r3, r4, r5, asr #16
    pkhtb r6, r7, r8, asr #24

    ite eq
    pkhtbeq r0, r1, r2, asr #8
    pkhtbne r3, r4, r5, asr #16