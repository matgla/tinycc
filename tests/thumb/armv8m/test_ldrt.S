.syntax unified
.thumb

.global _start 
_start:

.global test_ldrt
test_ldrt:
    ldrt r1, [r2, #32]
    ldrt r10, [r2]
   
    ite eq 
    ldrteq r1, [r2, #32]
    ldrtne r10, [r2]
    