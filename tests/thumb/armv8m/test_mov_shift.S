.syntax unified
.thumb

.global _start
_start:

.global test_mov_reg
test_mov_reg:
    movs r1, r1, asr r1
    movs r3, r3, lsl r8
    movs r0, r0, lsr r7
    movs r1, r1, ror r4

    ittee eq
    moveq r1, r1, asr r1
    moveq r3, r3, lsl r8
    movne r0, r0, lsr r7
    movne r1, r1, ror r4

    movs.w r1, r1, asr r1
    movs.w r3, r3, lsl r8
    movs.w r0, r0, lsr r7
    movs.w r1, r1, ror r4

    ittee eq
    moveq.w r1, r1, asr r1
    moveq.w r3, r3, lsl r8
    movne.w r0, r0, lsr r7
    movne.w r1, r1, ror r4


    movs r1, r2, asr r3
    movs r3, r1, lsl r8
    movs r0, r1, lsr r7
    movs r1, r2, ror r4

    ittee eq
    moveq r1, r2, asr r3
    moveq r3, r5, lsl r8
    movne r0, r1, lsr r7
    movne r1, r2, ror r4