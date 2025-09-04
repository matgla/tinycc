.syntax unified
.thumb

.global _start
_start:

.global test_sxtb
test_sxtb:
    sxtb r0, r1
    sxtb.w r0, r1
    sxtb r2, r3, ror #0
    sxtb r2, r3, ror #8
    sxtb r2, r3, ror #16
    sxtb r2, r3, ror #24
    sxtb r11, r10

    ittee eq
    sxtbeq r0, r1
    sxtbeq.w r0, r1
    sxtbne r2, r3, ror #8
    sxtbne r11, r10