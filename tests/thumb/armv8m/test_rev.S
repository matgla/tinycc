.syntax unified
.thumb

.global _start
_start:

.global test_rev
test_rev:
    rev r0, r1
    rev.w r0, r1
    rev r2, r3
    rev r4, r5
    rev r6, r7
    rev r8, r9
    rev r10, r11
    rev r12, lr