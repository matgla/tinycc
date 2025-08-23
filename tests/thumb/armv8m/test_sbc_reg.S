.syntax unified
.thumb

.global _start
_start:

.global test_sbc_reg
test_sbc_reg:
    // T1
    sbcs.n r2, r2, r5
    itte eq
    sbceq r1, r1, r2
    sbceq r3, r5
    sbcne r5, r5, r1

    sbc r1, r1, r4
    // T2
    sbc r1, r2, r3, rrx
    sbc r10, r11, r12, rrx
    itte eq
    sbceq r10, r11, r12
    sbceq r10, r12
    sbcne r11, r1

    sbc r1, r1, r2, lsl #1
    sbc r6, r6, r8, lsr #2
    sbc r1, r1, r2, asr #3
    sbc r6, r6, r8, ror #4

    sbc r1, r4, r2, lsl #1
    sbc r6, r4, r8, lsr #2
    sbc r1, r8, r2, asr #3
    sbc r6, r7, r8, ror #4


    sbcs r1, r2, r3, rrx
    sbcs r10, r11, r12, rrx

    sbcs r1, r1, r2, lsl #1
    sbcs r6, r6, r8, lsr #2
    sbcs r1, r1, r2, asr #3
    sbcs r6, r6, r8, ror #4

    sbcs r1, r4, r2, lsl #1
    sbcs r6, r4, r8, lsr #2
    sbcs r1, r8, r2, asr #3
    sbcs r6, r7, r8, ror #4

    ittee eq
    sbceq r1, r1, r2, lsl #1
    sbceq r6, r6, r8, lsr #2
    sbcne r1, r1, r2, asr #3
    sbcne r6, r6, r8, ror #4

    ittee eq
    sbcseq r1, r4, r2, lsl #1
    sbcseq r6, r4, r8, lsr #2
    sbcsne r1, r8, r2, asr #3
    sbcsne r6, r7, r8, ror #4
