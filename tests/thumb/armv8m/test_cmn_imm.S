.syntax unified
.thumb

.global _start 
_start:

.global test_cmn_imm
test_cmn_imm:
    cmn r2, #0xde 
    cmn r4, #0xad
    cmn r10, #0xa
    cmn r8, #0xb
    cmn r2, #0xfafafafa

    itte gt 
    cmngt r1, #1 
    cmngt r11, #2 
    cmnle r7, #2
    