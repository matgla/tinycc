.syntax unified
.thumb

.global _start
_start:

.global test_movt
test_movt:
    movt r0, #0xffff
    movt r11, #0xff1
    movt r2, #0x12
