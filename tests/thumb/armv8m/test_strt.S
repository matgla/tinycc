.syntax unified
.thumb

.global _start
_start:

.global test_strt
test_strt:
    strt r1, [r2, #32]
    strt r10, [r2]

    ite eq
    strteq r1, [r2, #32]
    strtne r10, [r2]
