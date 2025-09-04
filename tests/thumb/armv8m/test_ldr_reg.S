.syntax unified
.thumb

.global _start 
_start:

.global test_ldr
test_ldr:
    ldr r1, [r0, r3]
    ldr r2, [r0, r6]
    ldr r3, [r5, r10]
    ldr r10, [r5, r1]
    ldr r4, [r10, r2]
    
    ite eq
    ldreq r1, [r0, r2] 
    ldrne r2, [r0, r3]

    ldr r1, [sp, r2]
    ldr r2, [sp, r10]
    ldr r10, [sp, r1]

    ldr.w r1, [r0, r2]
    ldr r1, [r2, r4, lsl #1]

