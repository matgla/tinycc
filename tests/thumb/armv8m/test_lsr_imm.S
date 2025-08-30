.syntax unified
.thumb

.global _start 
_start:

.global test_lsr_imm
test_lsr_imm:
    lsr r0, r1, #0x7
    lsr.w r0, r1, #0x8
    lsr r11, r2, #0x4
    lsr r0, #0x1a
    lsr.w r0, #0x8
    lsr r11, #0x4

    ittee eq
    lsreq r0, #1 
    lsreq.w r1, #2
    lsrne r11, #4
    lsrne r11, r8, #3

    ite gt 
    lsrgt r1, r2, #2
    lsrle r2, r1, #2

    lsrs r0, r1, #0x7
    lsrs.w r0, r1, #0x8
    lsrs r11, r2, #0x4
    lsrs r0, #0x1a
    lsrs.w r0, #0x8
    lsrs r11, #0x4
