.syntax unified
.thumb

.global _start 
_start:

.global test_and_reg
test_and_reg:
    // T1 
    ands.n r2, r2, r5
    itte eq 
    andeq r1, r1, r2
    andeq r3, r5 
    andne r5, r5, r1

    and r1, r1, r4
    // T2
    and r1, r2, r3, rrx 
    and r10, r11, r12, rrx 
    itte eq
    andeq r10, r11, r12 
    andeq r10, r12
    andne r11, r1

    and r1, r1, r2, lsl #1
    and r6, r6, r8, lsr #2
    and r1, r1, r2, asr #3
    and r6, r6, r8, ror #4
    
    and r1, r4, r2, lsl #1
    and r6, r4, r8, lsr #2
    and r1, r8, r2, asr #3
    and r6, r7, r8, ror #4


    ands r1, r2, r3, rrx 
    ands r10, r11, r12, rrx 

    ands r1, r1, r2, lsl #1
    ands r6, r6, r8, lsr #2
    ands r1, r1, r2, asr #3
    ands r6, r6, r8, ror #4
    
    ands r1, r4, r2, lsl #1
    ands r6, r4, r8, lsr #2
    ands r1, r8, r2, asr #3
    ands r6, r7, r8, ror #4

    ittee eq
    andeq r1, r1, r2, lsl #1
    andeq r6, r6, r8, lsr #2
    andne r1, r1, r2, asr #3
    andne r6, r6, r8, ror #4
    
    ittee eq
    andseq r1, r4, r2, lsl #1
    andseq r6, r4, r8, lsr #2
    andsne r1, r8, r2, asr #3
    andsne r6, r7, r8, ror #4
