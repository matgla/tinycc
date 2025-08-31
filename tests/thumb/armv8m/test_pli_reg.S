.syntax unified
.thumb

.global _start 
_start:

.global test_pli_reg
test_pli_reg:
    pli [r1, r2, lsl #2]
    pli [r10, r11]

    ite eq
    plieq [r1, r10, lsl #3]
    pline [r10, r2]

