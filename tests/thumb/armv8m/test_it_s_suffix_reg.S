.syntax unified
.thumb

.global _start
_start:

.global test_it_s_suffix_reg
test_it_s_suffix_reg:
    it ne
    andsne r0, r0, r1
    it ne
    addsne r0, r1, r2
    it ne
    mvnsne r0, r1
    it ne
    orrsne r0, r0, r1
    it ne
    eorsne r0, r0, r1
    it ne
    adcsne r0, r0, r1
    it ne
    sbcsne r0, r0, r1
    it ne
    bicsne r0, r0, r1
    it ne
    subsne r0, r1, r2
    ite eq
    andseq r2, r2, r3
    orrsne r2, r2, r3
    bx lr
