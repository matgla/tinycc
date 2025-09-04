.syntax unified
.thumb

.global _start 
_start:

.global test_ldrsh
test_ldrsh:
    ldrsh r1, [r0, r3]
    ldrsh r2, [r0, r6]
    ldrsh r3, [r5, r10]
    ldrsh r10, [r5, r1]
    ldrsh r4, [r10, r2]
    
    ite eq
    ldrsheq r1, [r0, r2] 
    ldrshne r2, [r0, r3]

    ldrsh r1, [sp, r2]
    ldrsh r2, [sp, r10]
    ldrsh r10, [sp, r1]

    ldrsh.w r1, [r0, r2]
    ldrsh r1, [r2, r4, lsl #1]

