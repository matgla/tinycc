.syntax unified
.thumb

.global _start
_start:

.global test_mov_reg
test_mov_reg:
    mov r1, r2
    mov r3, r3
    mov r0, r0
    mov r10, r11

    movs r1, r2
    movs r3, r3
    movs r0, r0
    movs r10, r11

    ittee eq
    moveq r1, r2
    moveq r3, r3
    movne r0, r0
    movne r10, r11

    movs r1, r2, rrx
    movs r10, r2, lsl #1
    movs r6, r8, lsr #2
    movs r1, r10, asr #3
    movs r10, r11, ror #4

    ittee eq
    moveq r11, r2, lsl #1
    moveq r6, r8, lsr #2
    movne r1, r8, asr #3
    movne r10, r11, ror #4

    mov r0, r2, lsl #1
    movs r0, r2, lsl #1 // optimized as lsl

    mov r1, r3, asr #3
    movs r1, r3, asr #3 // optimized as asr

