.syntax unified
.thumb

.global _start 
_start:

.global test_cmp_imm
test_cmp_imm:
    cmp r0, #10
    cmp r1, #0xa0a0a0a0
    cmp.w r2, #10
    cmp r10, #0xffffffff

    ite eq 
    cmpeq r1, #1
    cmpne r2, #2

    ite gt
    cmpgt r10, #0xffffffff
    cmple r10, #0x00000000