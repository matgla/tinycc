.syntax unified
.thumb

.global _start
_start:

.global test_udiv
test_udiv:
    udiv r1, r2
    udiv r1, r2, r3

    ite eq
    udiveq r1, r2
    udivne r1, r2, r3

    udiv.w r1, r2
    udiv r1, r10
    udiv r7, r8, r9