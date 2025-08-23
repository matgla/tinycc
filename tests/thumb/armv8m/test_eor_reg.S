.syntax unified
.thumb

.global _start 
_start:

.global test_eor_reg
test_eor_reg:
    // T1 
    eors.n r2, r2, r5
    itte eq 
    eoreq r1, r1, r2
    eoreq r3, r5 
    eorne r5, r5, r1

    eor r1, r1, r4
    // T2
    eor r1, r2, r3, rrx 
    eor r10, r11, r12, rrx 
    itte eq
    eoreq r10, r11, r12 
    eoreq r10, r12
    eorne r11, r1

    eor r1, r1, r2, lsl #1
    eor r6, r6, r8, lsr #2
    eor r1, r1, r2, asr #3
    eor r6, r6, r8, ror #4
    
    eor r1, r4, r2, lsl #1
    eor r6, r4, r8, lsr #2
    eor r1, r8, r2, asr #3
    eor r6, r7, r8, ror #4


    eors r1, r2, r3, rrx 
    eors r10, r11, r12, rrx 

    eors r1, r1, r2, lsl #1
    eors r6, r6, r8, lsr #2
    eors r1, r1, r2, asr #3
    eors r6, r6, r8, ror #4
    
    eors r1, r4, r2, lsl #1
    eors r6, r4, r8, lsr #2
    eors r1, r8, r2, asr #3
    eors r6, r7, r8, ror #4

    ittee eq
    eoreq r1, r1, r2, lsl #1
    eoreq r6, r6, r8, lsr #2
    eorne r1, r1, r2, asr #3
    eorne r6, r6, r8, ror #4
    
    ittee eq
    eorseq r1, r4, r2, lsl #1
    eorseq r6, r4, r8, lsr #2
    eorsne r1, r8, r2, asr #3
    eorsne r6, r7, r8, ror #4
