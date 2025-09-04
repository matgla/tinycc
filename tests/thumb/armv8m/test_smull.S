.syntax unified
.thumb

.global _start
_start:

.global test_smull
test_smull:
    smull r0, r1, r2, r3
    smull r8, r9, r10, r11

    ite eq
    smulleq r0, r1, r2, r3
    smullne r8, r9, r10, r11