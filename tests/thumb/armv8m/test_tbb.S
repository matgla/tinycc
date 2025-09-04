.syntax unified
.thumb

.global _start
_start:

.global test_tbb
test_tbb:
    tbb [r0, r1]

    ite eq
    moveq r0, r0
    tbbne [r0, r1]

    tbh [r5, r1, lsl #1]
    tbh [r0, r1, lsl #1]

    ite eq
    moveq r0, r0
    tbhne [r12, r1, lsl #1]

