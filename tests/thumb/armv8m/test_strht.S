.syntax unified
.thumb

.global _start
_start:

.global test_strht
test_strht:
    strht r1, [r2, #32]
    strht r10, [r2]

    ite eq
    strhteq r1, [r2, #32]
    strhtne r10, [r2]
