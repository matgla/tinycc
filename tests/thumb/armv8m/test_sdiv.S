.syntax unified
.thumb

.global _start
_start:

.global test_sdiv
test_sdiv:
    sdiv r1, r2
    sdiv r1, r2, r3

    ite eq
    sdiveq r1, r2
    sdivne r1, r2, r3

    sdiv.w r1, r2
    sdiv r1, r10
    sdiv r7, r8, r9