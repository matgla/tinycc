.syntax unified
.thumb

.global _start
_start:

.global test_str
test_str:
    str r1, [r0, #16]
    str r2, [r0, #15]
    str r3, [r5, #10]
    str r2, [r1, #12]
    str r2, [r3]

    ittee eq
    streq r1, [r0, #110]
    streq r2, [r0, #15]
    strne r3, [r5, #10]
    strne r2, [r1, #12]

    str r1, [sp]
    str r2, [sp, #4]
    str r3, [sp, #4]

    itte gt
    strgt r1, [sp]
    strgt r2, [sp, #4]
    strle r3, [sp, #4]

    str.w r1, [r0, #110]
    str.w r2, [r0, #15]
    str.w r3, [r5, #10]
    str.w r2, [r1, #12]
    str.w r2, [r3]

    str.w r1, [sp]
    str.w r2, [sp, #4]
    str.w r3, [sp, #4]

    str r1, [r10, #110]
    str r12, [r0, #15]
    str r3, [r5, #10]
    str r11, [r10, #12]
    str r12, [r11]

    ittee eq
    streq r10, [r10, #110]
    streq r9, [r0, #15]
    strne r11, [r5, #10]
    strne r9, [r9, #12]

    str r10, [sp]
    str r12, [sp, #4]
    str r12, [sp, #-4]


    str r1, [r1, #-110]
    str r12, [r0, #-15]
    str r1, [r2], #10
    str r1, [r2], #-15
    str r1, [r3, #-110]!
    str r12, [r0, #-15]!

