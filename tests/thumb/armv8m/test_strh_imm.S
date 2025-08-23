.syntax unified
.thumb

.global _start
_start:

.global test_strh
test_strh:
    strh r1, [r0, #16]
    strh r2, [r0, #15]
    strh r3, [r5, #10]
    strh r2, [r1, #12]
    strh r2, [r3]

    ittee eq
    strheq r1, [r0, #110]
    strheq r2, [r0, #15]
    strhne r3, [r5, #10]
    strhne r2, [r1, #12]

    strh r1, [sp]
    strh r2, [sp, #4]
    strh r3, [sp, #4]

    itte gt
    strhgt r1, [sp]
    strhgt r2, [sp, #4]
    strhle r3, [sp, #4]

    strh.w r1, [r0, #110]
    strh.w r2, [r0, #15]
    strh.w r3, [r5, #10]
    strh.w r2, [r1, #12]
    strh.w r2, [r3]

    strh.w r1, [sp]
    strh.w r2, [sp, #4]
    strh.w r3, [sp, #4]

    strh r1, [r10, #110]
    strh r12, [r0, #15]
    strh r3, [r5, #10]
    strh r11, [r10, #12]
    strh r12, [r11]

    ittee eq
    strheq r10, [r10, #110]
    strheq r9, [r0, #15]
    strhne r11, [r5, #10]
    strhne r9, [r9, #12]

    strh r10, [sp]
    strh r12, [sp, #4]
    strh r12, [sp, #-4]


    strh r1, [r1, #-110]
    strh r12, [r0, #-15]
    strh r1, [r2], #10
    strh r1, [r2], #-15
    strh r1, [r3, #-110]!
    strh r12, [r0, #-15]!

