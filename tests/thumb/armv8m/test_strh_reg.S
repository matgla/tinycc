.syntax unified
.thumb

.global _start
_start:

.global test_strh
test_strh:
    strh r1, [r0, r3]
    strh r2, [r0, r6]
    strh r3, [r5, r10]
    strh r10, [r5, r1]
    strh r4, [r10, r2]

    ite eq
    strheq r1, [r0, r2]
    strhne r2, [r0, r3]

    strh r1, [sp, r2]
    strh r2, [sp, r10]
    strh r10, [sp, r1]

    strh.w r1, [r0, r2]
    strh r1, [r2, r4, lsl #1]

