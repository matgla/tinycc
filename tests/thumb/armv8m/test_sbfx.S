.syntax unified
.thumb

.global _start
_start:

.global test_sbfx
test_sbfx:
    sbfx r0, r1, #0, #1
    sbfx r2, r3, #16, #2
    sbfx r10, r11, #2, #20

    bx lr
    nop
