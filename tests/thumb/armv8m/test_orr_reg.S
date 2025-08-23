.syntax unified
.thumb

.global _start
_start:

.global test_orr_reg
test_orr_reg:
    // T1
    orrs r2, r2, r5
    itte eq
    orreq r1, r1, r2
    orreq r3, r5
    orrne r5, r5, r1

    orr r1, r1, r4
    // T2
    orr r1, r2, r3, rrx
    orr r10, r11, r12, rrx
    itte eq
    orreq r10, r11, r12
    orreq r10, r12
    orrne r11, r1

    orr r1, r1, r2, lsl #1
    orr r6, r6, r8, lsr #2
    orr r1, r1, r2, asr #3
    orr r6, r6, r8, ror #4

    orr r1, r4, r2, lsl #1
    orr r6, r4, r8, lsr #2
    orr r1, r8, r2, asr #3
    orr r6, r7, r8, ror #4


    orrs r1, r2, r3, rrx
    orrs r10, r11, r12, rrx

    orrs r1, r1, r2, lsl #1
    orrs r6, r6, r8, lsr #2
    orrs r1, r1, r2, asr #3
    orrs r6, r6, r8, ror #4

    orrs r1, r4, r2, lsl #1
    orrs r6, r4, r8, lsr #2
    orrs r1, r8, r2, asr #3
    orrs r6, r7, r8, ror #4

    ittee eq
    orreq r1, r1, r2, lsl #1
    orreq r6, r6, r8, lsr #2
    orrne r1, r1, r2, asr #3
    orrne r6, r6, r8, ror #4

    ittee eq
    orrseq r1, r4, r2, lsl #1
    orrseq r6, r4, r8, lsr #2
    orrsne r1, r8, r2, asr #3
    orrsne r6, r7, r8, ror #4
