.syntax unified
.thumb

.global _start
_start:

.global test_strb
test_strb:
    strb r1, [r0, #16]
    strb r2, [r0, #15]
    strb r3, [r5, #10]
    strb r2, [r1, #12]
    strb r2, [r3]

    ittee eq
    strbeq r1, [r0, #110]
    strbeq r2, [r0, #15]
    strbne r3, [r5, #10]
    strbne r2, [r1, #12]

    strb r1, [sp]
    strb r2, [sp, #4]
    strb r3, [sp, #4]

    itte gt
    strbgt r1, [sp]
    strbgt r2, [sp, #4]
    strble r3, [sp, #4]

    strb.w r1, [r0, #110]
    strb.w r2, [r0, #15]
    strb.w r3, [r5, #10]
    strb.w r2, [r1, #12]
    strb.w r2, [r3]

    strb.w r1, [sp]
    strb.w r2, [sp, #4]
    strb.w r3, [sp, #4]

    strb r1, [r10, #110]
    strb r12, [r0, #15]
    strb r3, [r5, #10]
    strb r11, [r10, #12]
    strb r12, [r11]

    ittee eq
    strbeq r10, [r10, #110]
    strbeq r9, [r0, #15]
    strbne r11, [r5, #10]
    strbne r9, [r9, #12]

    strb r10, [sp]
    strb r12, [sp, #4]
    strb r12, [sp, #-4]


    strb r1, [r1, #-110]
    strb r12, [r0, #-15]
    strb r1, [r2], #10
    strb r1, [r2], #-15
    strb r1, [r3, #-110]!
    strb r12, [r0, #-15]!

