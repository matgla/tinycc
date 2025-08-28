.syntax unified
.thumb

.global _start 
_start:

.global test_eor_imm
test_eor_imm:
    eor r2, #0xde 
    eors r4, #0xad
    eors r10, r11, #0xa
    eor r8, r7, #0xb
    eors r1, r2, #0xfafafafa
    eor r2, r3, #0xfafafafa

    itte gt 
    eorgt r1, r2, #1 
    eorgt r10, r11, #2 
    eorle r11, #2
    