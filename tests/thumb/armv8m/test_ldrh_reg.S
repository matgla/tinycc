.syntax unified
.thumb

.global _start 
_start:

.global test_ldrh
test_ldrh:
    ldrh r1, [r0, r3]
    ldrh r2, [r0, r6]
    ldrh r3, [r5, r10]
    ldrh r10, [r5, r1]
    ldrh r4, [r10, r2]
    
    ite eq
    ldrheq r1, [r0, r2] 
    ldrhne r2, [r0, r3]

    ldrh r1, [sp, r2]
    ldrh r2, [sp, r10]
    ldrh r10, [sp, r1]

    ldrh.w r1, [r0, r2]
    ldrh r1, [r2, r4, lsl #1]

