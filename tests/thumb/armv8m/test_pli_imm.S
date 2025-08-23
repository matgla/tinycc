.syntax unified
.thumb

.global _start 
_start:

.global test_pli_imm
test_pli_imm:
    pli [r1, #120]
    pli [r10, #10]
    pli [r7]
    pli [r2, #32]
    pli [r8, #-48]

    ittee eq
    plieq [r1, #120]
    plieq [r10, #10]
    pline [r7]
    pline [r2, #32]

    it gt
    pligt [r8, #-48]
