.syntax unified
.thumb

.global _start
_start:

.global test_orn_imm
test_orn_imm:
    orn r2, #0xde
    orns r4, #0xad
    orns r10, r11, #0xa
    orn r8, r7, #0xb
    orns r1, r2, #0xfafafafa
    orn r2, r3, #0xfafafafa

    itte gt
    orngt r1, r2, #1
    orngt r10, r11, #2
    ornle r11, #2
