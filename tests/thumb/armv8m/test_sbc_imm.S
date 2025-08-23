.syntax unified
.thumb

.global _start
_start:

.global test_sbc_imm
test_sbc_imm:
    sbc r2, #0xde
    sbcs r4, #0xad
    sbcs r10, r11, #0xa
    sbc r8, r7, #0xb
    sbcs r1, r2, #0xfafafafa
    sbc r2, r3, #0xfafafafa

    itte gt
    sbcgt r1, r2, #1
    sbcgt r10, r11, #2
    sbcle r11, #2
