.syntax unified
.thumb

.global _start
_start:

.global test_orr_imm
test_orr_imm:
    orr r2, #0xde
    orrs r4, #0xad
    orrs r10, r11, #0xa
    orr r8, r7, #0xb
    orrs r1, r2, #0xfafafafa
    orr r2, r3, #0xfafafafa

    itte gt
    orrgt r1, r2, #1
    orrgt r10, r11, #2
    orrle r11, #2
