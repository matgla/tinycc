.syntax unified
.thumb

.global _start 
_start:

.global test_bic_imm
test_bic_imm:
    bic r2, #0xde 
    bics r4, #0xad
    bics r10, r11, #0xa
    bic r8, r7, #0xb
    bics r1, r2, #0xfafafafa
    bic r2, r3, #0xfafafafa

    itte gt 
    bicgt r1, r2, #1 
    bicgt r10, r11, #2 
    bicle r11, #2
    