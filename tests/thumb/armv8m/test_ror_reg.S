.syntax unified
.thumb

.global _start
_start:

.global test_ror_reg
test_ror_reg:
    ror r0, r1
    ror r3, r3, r6
    ror.w r0, r1
    ror.w r3, r3, r6

    ror r11, r12, r10
    ror r1, r12

    ittee eq
    roreq r0, r1
    roreq r3, r3, r6
    rorne r11, r12, r10
    rorne r1, r12

    itt eq
    roreq.w r0, r1
    roreq.w r3, r3, r6

    rors r0, r1
    rors r3, r3, r6
    rors.w r0, r1
    rors.w r3, r3, r6

    rors r11, r12, r10
    rors r1, r12
