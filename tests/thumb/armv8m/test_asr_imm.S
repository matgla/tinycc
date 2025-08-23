.syntax unified
.thumb

.global _start 
_start:

.global test_asr_imm
test_asr_imm:
    asr r0, r1, #0x7
    asr.w r0, r1, #0x8
    asr r11, r2, #0x4
    asr r0, #0x1a
    asr.w r0, #0x8
    asr r11, #0x4

    ittee eq
    asreq r0, #1 
    asreq.w r1, #2
    asrne r11, #4
    asrne r11, r8, #3

    ite gt 
    asrgt r1, r2, #2
    asrle r2, r1, #2

    asrs r0, r1, #0x7
    asrs.w r0, r1, #0x8
    asrs r11, r2, #0x4
    asrs r0, #0x1a
    asrs.w r0, #0x8
    asrs r11, #0x4
