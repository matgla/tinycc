.syntax unified
.thumb

.global _start 
_start:

.global jump_target
jump_target:
    clz r8, r7
    clz r1, r2 
    ite eq 
    clzeq r1, r2 
    clzne r10, r11
