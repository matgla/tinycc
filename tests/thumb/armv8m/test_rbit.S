.syntax unified
.thumb

.global _start
_start:

.global test_rbit
test_rbit:
    rbit r0, r1
    rbit r2, r3
    rbit r4, r5
    rbit r6, r7
    rbit r8, r9
    rbit r10, r11
    rbit r12, lr