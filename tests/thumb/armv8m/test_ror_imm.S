.syntax unified
.thumb

.global _start
_start:

.global test_ror_imm
test_ror_imm:
    ror r0, r1, #0x7
    ror.w r0, r1, #0x8
    ror r11, r2, #0x4
    ror r0, #0x1a
    ror.w r0, #0x8
    ror r11, #0x4

    ittee eq
    roreq r0, #1
    roreq.w r1, #2
    rorne r11, #4
    rorne r11, r8, #3

    ite gt
    rorgt r1, r2, #2
    rorle r2, r1, #2

    rors r0, r1, #0x7
    rors.w r0, r1, #0x8
    rors r11, r2, #0x4
    rors r0, #0x1a
    rors.w r0, #0x8
    rors r11, #0x4
