.syntax unified
.thumb

.global _start 
_start:

.global test_bfi
test_bfi:
    bfi r0, r4, #0, #1
    bfi r2, r8, #16, #2
    bfi r10, r11, #2, #20
    