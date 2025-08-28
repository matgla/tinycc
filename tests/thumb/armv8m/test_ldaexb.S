.syntax unified
.thumb

.global _start 
_start:

.global test_ldaexb
test_ldaexb:
    ldaexb r1, [r2]
    ldaexb r9, [r10] 
    ldaexb r1, [r11] 

    ite eq
    ldaexbeq r2, [r3]
    ldaexbne r10, [r12]