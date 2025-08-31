.syntax unified
.thumb

.global _start
_start:

.global test_orn_reg
test_orn_reg:
    // T1
    orns r2, r2, r5
    itte eq
    orneq r1, r1, r2
    orneq r3, r5
    ornne r5, r5, r1

    orn r1, r1, r4
    // T2
    orn r1, r2, r3, rrx
    orn r10, r11, r12, rrx
    itte eq
    orneq r10, r11, r12
    orneq r10, r12
    ornne r11, r1

    orn r1, r1, r2, lsl #1
    orn r6, r6, r8, lsr #2
    orn r1, r1, r2, asr #3
    orn r6, r6, r8, ror #4

    orn r1, r4, r2, lsl #1
    orn r6, r4, r8, lsr #2
    orn r1, r8, r2, asr #3
    orn r6, r7, r8, ror #4


    orns r1, r2, r3, rrx
    orns r10, r11, r12, rrx

    orns r1, r1, r2, lsl #1
    orns r6, r6, r8, lsr #2
    orns r1, r1, r2, asr #3
    orns r6, r6, r8, ror #4

    orns r1, r4, r2, lsl #1
    orns r6, r4, r8, lsr #2
    orns r1, r8, r2, asr #3
    orns r6, r7, r8, ror #4

    ittee eq
    orneq r1, r1, r2, lsl #1
    orneq r6, r6, r8, lsr #2
    ornne r1, r1, r2, asr #3
    ornne r6, r6, r8, ror #4

    ittee eq
    ornseq r1, r4, r2, lsl #1
    ornseq r6, r4, r8, lsr #2
    ornsne r1, r8, r2, asr #3
    ornsne r6, r7, r8, ror #4
