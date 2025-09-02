.syntax unified
.thumb

.global _start
_start:

.global test_str
test_str:
    str r1, [r0, r3]
    str r2, [r0, r6]
    str r3, [r5, r10]
    str r10, [r5, r1]
    str r4, [r10, r2]

    ite eq
    streq r1, [r0, r2]
    strne r2, [r0, r3]

    str r1, [sp, r2]
    str r2, [sp, r10]
    str r10, [sp, r1]

    str.w r1, [r0, r2]
    str r1, [r2, r4, lsl #1]

