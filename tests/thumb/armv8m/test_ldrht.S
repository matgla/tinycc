.syntax unified
.thumb

.global _start 
_start:

.global test_ldrht
test_ldrht:
    ldrht r1, [r2, #32]
    ldrht r10, [r2]
   
    ite eq 
    ldrhteq r1, [r2, #32]
    ldrhtne r10, [r2]
    