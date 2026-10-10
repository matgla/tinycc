.syntax unified
.thumb
.global _start
_start:
.global test_it_s_suffix_shift_mov
test_it_s_suffix_shift_mov:
    it ne
    movsne r0, r1
    it ne
    movsne r0, #5
    it ne
    lslsne r0, r1, #2
    it ne
    lsrsne r2, r3, #3
    it ne
    asrsne r0, r1, #2
    it ne
    rorsne r0, r1, #4
    it ne
    lslsne r0, r0, r1
    it ne
    lsrsne r0, r0, r1
    it ne
    asrsne r0, r0, r1
    it ne
    rorsne r0, r0, r1
    it ne
    rrxsne r0, r1
    ite eq
    movseq r2, r3
    lslsne r2, r2, #1
    bx lr
