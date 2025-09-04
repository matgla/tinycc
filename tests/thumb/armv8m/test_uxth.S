.syntax unified
.thumb

.global _start
_start:

.global test_uxth
test_uxth:
    uxth r0, r1
    uxth.w r0, r1
    uxth r2, r3, ror #0
    uxth r2, r3, ror #8
    uxth r2, r3, ror #16
    uxth r2, r3, ror #24
    uxth r11, r10

    ittee eq
    uxtheq r0, r1
    uxtheq.w r0, r1
    uxthne r2, r3, ror #8
    uxthne r11, r10