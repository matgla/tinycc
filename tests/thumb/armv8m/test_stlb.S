.syntax unified
.thumb

.global _start
_start:

.global test_stlb
test_stlb:
    stlb r1, [r2]
    stlb r9, [r10]
    stlb r1, [r11]

    ite eq
    stlbeq r2, [r3]
    stlbne r10, [r12]