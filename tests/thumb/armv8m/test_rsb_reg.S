.syntax unified
.thumb

.global _start
_start:

.global test_rsb_reg
test_rsb_reg:
    // T1
    rsbs r2, r2, r5
    itte eq
    rsbeq r1, r1, r2
    rsbeq r3, r5
    rsbne r5, r5, r1

    rsb r1, r1, r4
    // T2
    rsb r1, r2, r3, rrx
    rsb r10, r11, r12, rrx
    itte eq
    rsbeq r10, r11, r12
    rsbeq r10, r12
    rsbne r11, r1

    rsb r1, r1, r2, lsl #1
    rsb r6, r6, r8, lsr #2
    rsb r1, r1, r2, asr #3
    rsb r6, r6, r8, ror #4

    rsb r1, r4, r2, lsl #1
    rsb r6, r4, r8, lsr #2
    rsb r1, r8, r2, asr #3
    rsb r6, r7, r8, ror #4


    rsbs r1, r2, r3, rrx
    rsbs r10, r11, r12, rrx

    rsbs r1, r1, r2, lsl #1
    rsbs r6, r6, r8, lsr #2
    rsbs r1, r1, r2, asr #3
    rsbs r6, r6, r8, ror #4

    rsbs r1, r4, r2, lsl #1
    rsbs r6, r4, r8, lsr #2
    rsbs r1, r8, r2, asr #3
    rsbs r6, r7, r8, ror #4

    ittee eq
    rsbeq r1, r1, r2, lsl #1
    rsbeq r6, r6, r8, lsr #2
    rsbne r1, r1, r2, asr #3
    rsbne r6, r6, r8, ror #4

    ittee eq
    rsbseq r1, r4, r2, lsl #1
    rsbseq r6, r4, r8, lsr #2
    rsbsne r1, r8, r2, asr #3
    rsbsne r6, r7, r8, ror #4
