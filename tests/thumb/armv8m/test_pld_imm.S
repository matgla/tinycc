.syntax unified
.thumb

.global _start 
_start:

.global test_pld_imm
test_pld_imm:
    pld [r1, #120]
    pld [r10, #10]
    pld [r7]
    pld [r2, #32]
    pld [r8, #-48]

    ittee eq
    pldeq [r1, #120]
    pldeq [r10, #10]
    pldne [r7]
    pldne [r2, #32]

    it gt
    pldgt [r8, #-48]
