.syntax unified
.thumb

.global _start
_start:

.global test_teq
test_teq:
    teq r2, #0
    teq r3, #255
    teq r4, #256
    teq r6, #512
    teq r8, #1024
    teq r10, #2048

    ite eq
    teqeq r0, #10
    teqne r12, #0xffffffff

