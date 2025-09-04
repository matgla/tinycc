.syntax unified
.thumb

.global _start
_start:

.global test_tst_reg
test_tst_reg:
    tst r2, r3
    tst.w r2, r3
    tst r3, r10
    tst r4, r8
    tst r6, r1, rrx
    tst r8, r1, lsl #4
    tst r8, r1, asr #4

    ite eq
    tsteq r0, r2
    tstne r12, r8

