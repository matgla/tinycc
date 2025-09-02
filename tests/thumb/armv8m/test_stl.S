.syntax unified
.thumb

.global _start
_start:

.global test_stl
test_stl:
    stl r1, [r2]
    stl r9, [r10]
    stl r1, [r11]

    ite eq
    stleq r2, [r3]
    stlne r10, [r12]