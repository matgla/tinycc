.syntax unified
.thumb

.global _start
_start:

.global test_push
test_push:
    push {r0, r1, r2, r4, r5, r6, r7}
    push {r0, r2, r4, r7}
    push {r0-r8}
    push {r2}
    push {lr}
