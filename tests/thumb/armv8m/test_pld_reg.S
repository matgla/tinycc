.syntax unified
.thumb

.global _start 
_start:

.global test_pld_reg
test_pld_reg:
    pld [r1, r2, lsl #2]
    pld [r10, r11]

    ite eq
    pldeq [r1, r10, lsl #3]
    pldne [r10, r2]

