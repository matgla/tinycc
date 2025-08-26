.syntax unified
.thumb

.global _start 
_start:

.global test_and_imm
test_and_imm:
    and r2, #0xde 
    ands r4, #0xad
    ands r10, r11, #0xa
    and r8, r7, #0xb
    ands r1, r2, #0xfafafafa
    and r2, r3, #0xfafafafa

    itte gt 
    andgt r1, r2, #1 
    andgt r10, r11, #2 
    andle r11, #2
    