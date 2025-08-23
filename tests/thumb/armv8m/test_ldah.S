.syntax unified
.thumb

.global _start 
_start:

.global test_ldah 
test_ldah:
    ldah r1, [r2]
    ldah r9, [r10] 
    ldah r1, [r11] 

    ite eq
    ldaheq r2, [r3]
    ldahne r10, [r12]