.syntax unified
.thumb

.global _start
_start:

.global test_thop_cmp
test_thop_cmp:
    /* T16 forms */
    cmp r0, #0xFF
    cmn r1, #0x10
    tst r2, #0x01
    teq r3, #0x02

    /* T32 forms */
    cmp r4, #0x12345678
    cmn r5, #0x87654321
    tst r6, #0xABCDEF00
    teq r7, #0x000000FF

    /* T32 with shift */
    cmp r8, r9, lsl #2
    cmn r10, r11, lsr #1

    /* IT blocks */
    ite eq
    cmpeq r0, #0x1
    cmpne r0, #0x2

    bx lr
