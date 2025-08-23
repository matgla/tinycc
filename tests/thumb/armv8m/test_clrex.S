.syntax unified
.thumb

.global _start 
_start:

.global jump_target
jump_target:
    clrex

test_b:
    movs r1, #2

next_target:
    clrex