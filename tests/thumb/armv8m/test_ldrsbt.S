.syntax unified
.thumb

.global _start 
_start:

.global test_ldrsbt
test_ldrsbt:
    ldrsbt r1, [r2, #32]
    ldrsbt r10, [r2]
   
    ite eq 
    ldrsbteq r1, [r2, #32]
    ldrsbtne r10, [r2]
    