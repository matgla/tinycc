.syntax unified
.thumb

.global _start 
_start:

.global test_asr_reg
test_asr_reg:
    asr r0, r1
    asr r3, r3, r6
    asr.w r0, r1
    asr.w r3, r3, r6
 
    asr r11, r12, r10
    asr r1, r12

    ittee eq
    asreq r0, r1
    asreq r3, r3, r6
    asrne r11, r12, r10
    asrne r1, r12

    itt eq
    asreq.w r0, r1
    asreq.w r3, r3, r6

    asrs r0, r1
    asrs r3, r3, r6
    asrs.w r0, r1
    asrs.w r3, r3, r6
 
    asrs r11, r12, r10
    asrs r1, r12


