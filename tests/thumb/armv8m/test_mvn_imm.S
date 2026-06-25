.syntax unified
.thumb

.global _start
_start:

.global test_mvn_imm
test_mvn_imm:
    mvn ip, #0
    mvn r2, #0xde
    mvns r4, #0xad
    mvns r10, #0xa
    mvn r8, #0xb
    mvns r1, #0xfafafafa
    mvn r2, #0xfafafafa

    itte gt
    mvngt r1, #1
    mvngt r10, #2
    mvnle r11, #2
