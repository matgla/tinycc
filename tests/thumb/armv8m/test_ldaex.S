.syntax unified
.thumb

.global _start 
_start:

.global test_ldaex
test_ldaex:
    ldaex r1, [r2]
    ldaex r9, [r10] 
    ldaex r1, [r11] 

    ite eq
    ldaexeq r2, [r3]
    ldaexne r10, [r12]