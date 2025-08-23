.syntax unified
.thumb

.global _start 
_start:

.global test_lsl_imm
test_lsl_imm:
    lsl r0, r1, #0x7
    lsl.w r0, r1, #0x8
    lsl r11, r2, #0x4
    lsl r0, #0x1a
    lsl.w r0, #0x8
    lsl r11, #0x4

    ittee eq
    lsleq r0, #1 
    lsleq.w r1, #2
    lslne r11, #4
    lslne r11, r8, #3

    ite gt 
    lslgt r1, r2, #2
    lslle r2, r1, #2

    lsls r0, r1, #0x7
    lsls.w r0, r1, #0x8
    lsls r11, r2, #0x4
    lsls r0, #0x1a
    lsls.w r0, #0x8
    lsls r11, #0x4
