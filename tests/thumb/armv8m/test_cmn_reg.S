.syntax unified
.thumb

.global _start 
_start:

.global test_cmn_reg
test_cmn_reg:
    // T1 
    cmn.n r2, r2
    itte eq 
    cmneq r1, r2
    cmneq r3, r5 
    cmnne r5, r1

    cmn r1, r1
    // T2
    cmn r1, r3, rrx 
    cmn r10, r12, rrx 
    itte eq
    cmneq r10, r12 
    cmneq r10, r12
    cmnne r11, r1

    cmn r1, r2, lsl #1
    cmn r6, r8, lsr #2
    cmn r1, r2, asr #3
    cmn r6, r8, ror #4
    
    cmn r1, r2, lsl #1
    cmn r6, r8, lsr #2
    cmn r1, r2, asr #3
    cmn r6, r8, ror #4

    ittee eq
    cmneq r1, r2, lsl #1
    cmneq r6, r8, lsr #2
    cmnne r1, r2, asr #3
    cmnne r6, r8, ror #4
    
