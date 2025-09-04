.syntax unified
.thumb

.global _start
_start:

.global test_strbt
test_strbt:
    strbt r1, [r2, #32]
    strbt r10, [r2]

    ite eq
    strbteq r1, [r2, #32]
    strbtne r10, [r2]
