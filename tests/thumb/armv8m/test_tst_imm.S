.syntax unified
.thumb

.global _start
_start:

.global test_tst
test_tst:
    tst r2, #0
    tst r3, #255
    tst r4, #256
    tst r6, #512
    tst r8, #1024
    tst r10, #2048

    ite eq
    tsteq r0, #10
    tstne r12, #0xffffffff

