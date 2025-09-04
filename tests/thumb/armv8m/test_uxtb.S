.syntax unified
.thumb

.global _start
_start:

.global test_uxtb
test_uxtb:
    uxtb r0, r1
    uxtb.w r0, r1
    uxtb r2, r3, ror #0
    uxtb r2, r3, ror #8
    uxtb r2, r3, ror #16
    uxtb r2, r3, ror #24
    uxtb r11, r10

    ittee eq
    uxtbeq r0, r1
    uxtbeq.w r0, r1
    uxtbne r2, r3, ror #8
    uxtbne r11, r10