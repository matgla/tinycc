.syntax unified
.thumb

.global _start
_start:

.global test_rev16
test_rev16:
    rev16 r0, r1
    rev16.w r0, r1
    rev16 r2, r3
    rev16 r4, r5
    rev16 r6, r7
    rev16 r8, r9
    rev16 r10, r11
    rev16 r12, lr