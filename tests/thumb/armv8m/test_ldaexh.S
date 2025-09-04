.syntax unified
.thumb

.global _start 
_start:

.global test_ldaexh
test_ldaexh:
    ldaexh r1, [r2]
    ldaexh r9, [r10] 
    ldaexh r1, [r11] 

    ite eq
    ldaexheq r2, [r3]
    ldaexhne r10, [r12]