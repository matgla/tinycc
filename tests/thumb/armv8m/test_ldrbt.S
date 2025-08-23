.syntax unified
.thumb

.global _start 
_start:

.global test_ldrbt
test_ldrbt:
    ldrbt r1, [r2, #32]
    ldrbt r10, [r2]
   
    ite eq 
    ldrbteq r1, [r2, #32]
    ldrbtne r10, [r2]
    