.syntax unified
.thumb

.global _start 
_start:

.global test_lsr_reg
test_lsr_reg:
    lsr r0, r1
    lsr r3, r3, r6
    lsr.w r0, r1
    lsr.w r3, r3, r6
 
    lsr r11, r12, r10
    lsr r1, r12

    ittee eq
    lsreq r0, r1
    lsreq r3, r3, r6
    lsrne r11, r12, r10
    lsrne r1, r12

    itt eq
    lsreq.w r0, r1
    lsreq.w r3, r3, r6

    lsrs r0, r1
    lsrs r3, r3, r6
    lsrs.w r0, r1
    lsrs.w r3, r3, r6
 
    lsrs r11, r12, r10
    lsrs r1, r12


