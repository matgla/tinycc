.syntax unified
.thumb

.global _start
_start:

.global test_mul
test_mul:
    muls r1, r2
    mul r1, r2, r3

    ite eq
    muleq r1, r2
    mulne r1, r2, r3

    mul.w r1, r2
    mul r1, r10
    mul r7, r8, r9