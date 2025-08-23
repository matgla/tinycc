.syntax unified
.thumb

.global _start 
_start:

.global test_ldrex
test_ldrex:
    ldrex r8, [r10]
    ldrex r1, [r2, #8]
    ldrex r8, [r1, #32] 

    itee eq 
    ldrexeq r8, [r10]
    ldrexne r1, [r2, #8]
    ldrexne r8, [r1, #32] 