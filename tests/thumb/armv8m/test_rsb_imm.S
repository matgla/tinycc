.syntax unified
.thumb

.global _start
_start:

.global test_rsb_imm
test_rsb_imm:
    rsb r2, #0
    rsbs r4, #0xad
    rsbs r10, r11, #0xa
    rsb r8, r7, #0xb
    rsbs r1, r2, #0xfafafafa
    rsb r2, r3, #0xfafafafa

    itte gt
    rsbgt.w r1, r2, #0
    rsbgt r10, r11, #2
    rsble r11, #2
