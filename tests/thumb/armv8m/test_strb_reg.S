.syntax unified
.thumb

.global _start
_start:

.global test_strb
test_strb:
    strb r1, [r0, r3]
    strb r2, [r0, r6]
    strb r3, [r5, r10]
    strb r10, [r5, r1]
    strb r4, [r10, r2]

    ite eq
    strbeq r1, [r0, r2]
    strbne r2, [r0, r3]

    strb r1, [sp, r2]
    strb r2, [sp, r10]
    strb r10, [sp, r1]

    strb.w r1, [r0, r2]
    strb r1, [r2, r4, lsl #1]

