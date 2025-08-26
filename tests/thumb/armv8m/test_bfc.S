.syntax unified
.thumb

.global _start 
_start:

.global test_bfc 
test_bfc:
    bfc r0, #0, #1
    bfc r2, #16, #2
    bfc r10, #2, #20
    