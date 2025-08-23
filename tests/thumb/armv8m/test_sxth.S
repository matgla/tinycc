.syntax unified
.thumb

.global _start
_start:

.global test_sxth
test_sxth:
    sxth r0, r1
    sxth.w r0, r1
    sxth r2, r3, ror #0
    sxth r2, r3, ror #8
    sxth r2, r3, ror #16
    sxth r2, r3, ror #24
    sxth r11, r10

    ittee eq
    sxtheq r0, r1
    sxtheq.w r0, r1
    sxthne r2, r3, ror #8
    sxthne r11, r10