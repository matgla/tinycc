.syntax unified
.thumb

.global _start
_start:

.global test_revsh
test_revsh:
    revsh r0, r1
    revsh.w r0, r1
    revsh r2, r3
    revsh r4, r5
    revsh r6, r7
    revsh r8, r9
    revsh r10, r11
    revsh r12, lr