.syntax unified
.thumb

.global _start 
_start:

.global test_ldab 
test_ldab:
    ldab r1, [r2]
    ldab r9, [r10] 
    ldab r1, [r11] 

    ite eq
    ldabeq r2, [r3]
    ldabne r10, [r12]