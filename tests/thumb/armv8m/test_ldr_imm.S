.syntax unified
.thumb

.global _start
_start:

.global test_ldr
test_ldr:
    ldr r1, [r0, #16]
    ldr r2, [r0, #15]
    ldr r3, [r5, #10]
    ldr r2, [r1, #12]
    ldr r2, [r3]

    ittee eq
    ldreq r1, [r0, #110]
    ldreq r2, [r0, #15]
    ldrne r3, [r5, #10]
    ldrne r2, [r1, #12]

    ldr r1, [sp]
    ldr r2, [sp, #4]
    ldr r3, [sp, #4]

    itte gt
    ldrgt r1, [sp]
    ldrgt r2, [sp, #4]
    ldrle r3, [sp, #4]

    ldr.w r1, [r0, #110]
    ldr.w r2, [r0, #15]
    ldr.w r3, [r5, #10]
    ldr.w r2, [r1, #12]
    ldr.w r2, [r3]

    ldr.w r1, [sp]
    ldr.w r2, [sp, #4]
    ldr.w r3, [sp, #4]

    ldr r1, [r10, #110]
    ldr r12, [r0, #15]
    ldr r3, [r5, #10]
    ldr r11, [r10, #12]
    ldr r12, [r11]

    ittee eq
    ldreq r10, [r10, #110]
    ldreq r9, [r0, #15]
    ldrne r11, [r5, #10]
    ldrne r9, [r9, #12]

    ldr r10, [sp]
    ldr r12, [sp, #4]
    ldr r12, [sp, #-4]


    ldr r1, [r1, #-110]
    ldr r12, [r0, #-15]
    ldr r1, [r2], #10
    ldr r1, [r2], #-15
    ldr r1, [r3, #-110]!
    ldr r12, [r0, #-15]!
