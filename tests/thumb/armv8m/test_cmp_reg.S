.syntax unified
.thumb

.global _start 
_start:

.global test_cmp_reg
test_cmp_reg:
    cmp r0, r1
    cmp r2, r2
    cmp r5, r6

    ite eq 
    cmpeq r0, r1
    cmpne r2, r2

    cmp r10, r12
    cmp r1, r9
    cmp r10, r11
    
    ite eq
    cmpeq r10, r12
    cmpne r1, r9

    cmp r1, r2, rrx 
    cmp r11, r12, rrx 
    
    itte eq
    cmpeq r10, r11, rrx
    cmpeq r1, r2, rrx
    cmpne r10, r12

    cmp r1, r2, lsl #1
    cmp r6, r8, lsr #2
    cmp r1, r2, asr #3
    cmp r6, r8, ror #4