.syntax unified
.thumb

.text
.global _start
.type _start, %function
_start:
    adr r0, adr_target_100
    adr r1, adr_target_300 + 8
    adr r2, adr_target_900 - 4
    ldr r3, ldr_target_100
    ldr r4, ldr_target_300 + 0xf0
    ldrd r5, r6, adr_target_100 + 12
    bx lr
    nop

    .space 0x100
adr_target_100:
    .word 0x11111111
    .space 0x200
adr_target_300:
    .word 0x22222222
    .space 0x600
adr_target_900:
    .word 0x33333333

    .space 0x100
ldr_target_100:
    .word 0x44444444
    .space 0x200
ldr_target_300:
    .word 0x55555555
