.syntax unified
.thumb

.global _start
_start:

.global test_mvn_reg
test_mvn_reg:
    // T1
    mvns.n r2, r5
    itte eq
    mvneq r1, r2
    mvneq r3, r5
    mvnne r5, r1

    mvn r1, r4
    // T2
    mvn r1, r2, rrx
    mvn r10, r11, rrx
    itte eq
    mvneq r10, r11
    mvneq r10, r12
    mvnne r11, r1

    mvn r1, r2, lsl #1
    mvn r6, r8, lsr #2
    mvn r1, r2, asr #3
    mvn r6, r8, ror #4

    mvn r1, r2, lsl #1
    mvn r6, r8, lsr #2
    mvn r1, r2, asr #3
    mvn r6, r8, ror #4


    mvns r1, r3, rrx
    mvns r10, r12, rrx

    mvns r1, r2, lsl #1
    mvns r6, r8, lsr #2
    mvns r1, r2, asr #3
    mvns r6, r8, ror #4

    mvns r1, r2, lsl #1
    mvns r6, r8, lsr #2
    mvns r1, r2, asr #3
    mvns r6, r8, ror #4

    ittee eq
    mvneq r1, r2, lsl #1
    mvneq r6, r8, lsr #2
    mvnne r1, r2, asr #3
    mvnne r6, r8, ror #4

    ittee eq
    mvnseq r1, r2, lsl #1
    mvnseq r6, r8, lsr #2
    mvnsne r1, r2, asr #3
    mvnsne r6, r8, ror #4
