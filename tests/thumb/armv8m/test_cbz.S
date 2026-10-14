.syntax unified
.thumb

.global _start 
_start:

.global jump_target
jump_target:
    movs r0, #0
    cmp r1, #1

    cmp r0, #1 
    cbnz r2, test_b

    cmp r0, #2
    cbz r2, test_b 
    nop

test_b:
    cbz r1, next_target 
    cbnz r3, next_target
    nop @ cbz offset is relative to pc+4: a target at +2 is not encodable

next_target:
    movs r1, #1
