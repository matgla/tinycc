.syntax unified
.thumb

.global _start 
_start:

.global test_lsl_reg
test_lsl_reg:
    lsl r0, r1
    lsl r3, r3, r6
    lsl.w r0, r1
    lsl.w r3, r3, r6
 
    lsl r11, r12, r10
    lsl r1, r12

    ittee eq
    lsleq r0, r1
    lsleq r3, r3, r6
    lslne r11, r12, r10
    lslne r1, r12

    itt eq
    lsleq.w r0, r1
    lsleq.w r3, r3, r6

    lsls r0, r1
    lsls r3, r3, r6
    lsls.w r0, r1
    lsls.w r3, r3, r6
 
    lsls r11, r12, r10
    lsls r1, r12


