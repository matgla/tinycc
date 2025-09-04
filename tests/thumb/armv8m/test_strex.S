.syntax unified
.thumb

.global _start
_start:

.global test_strex
test_strex:
    strex r8, r9, [r10]
    strex r1, r3, [r2, #8]
    strex r8, r4, [r1, #32]

    itee eq
    strexeq r8, r1, [r10]
    strexne r1, r3, [r2, #8]
    strexne r8, r5, [r1, #32]