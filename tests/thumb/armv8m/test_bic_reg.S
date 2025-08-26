.syntax unified
.thumb

.global _start 
_start:

.global test_bic_reg
test_bic_reg:
    // T1 
    bics.n r2, r2, r5
    itte eq 
    biceq r1, r1, r2
    biceq r3, r5 
    bicne r5, r5, r1

    bic r1, r1, r4
    // T2
    bic r1, r2, r3, rrx 
    bic r10, r11, r12, rrx 
    itte eq
    biceq r10, r11, r12 
    biceq r10, r12
    bicne r11, r1

    bic r1, r1, r2, lsl #1
    bic r6, r6, r8, lsr #2
    bic r1, r1, r2, asr #3
    bic r6, r6, r8, ror #4
    
    bic r1, r4, r2, lsl #1
    bic r6, r4, r8, lsr #2
    bic r1, r8, r2, asr #3
    bic r6, r7, r8, ror #4


    bics r1, r2, r3, rrx 
    bics r10, r11, r12, rrx 

    bics r1, r1, r2, lsl #1
    bics r6, r6, r8, lsr #2
    bics r1, r1, r2, asr #3
    bics r6, r6, r8, ror #4
    
    bics r1, r4, r2, lsl #1
    bics r6, r4, r8, lsr #2
    bics r1, r8, r2, asr #3
    bics r6, r7, r8, ror #4

    ittee eq
    biceq r1, r1, r2, lsl #1
    biceq r6, r6, r8, lsr #2
    bicne r1, r1, r2, asr #3
    bicne r6, r6, r8, ror #4
    
    ittee eq
    bicseq r1, r4, r2, lsl #1
    bicseq r6, r4, r8, lsr #2
    bicsne r1, r8, r2, asr #3
    bicsne r6, r7, r8, ror #4
