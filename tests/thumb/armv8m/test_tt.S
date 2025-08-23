.syntax unified
.thumb

.global _start
_start:

.global test_tt
test_tt:
    tt r8, r10
    tta r1, r2
    ttat r2, r8
    ttt r1, r8
