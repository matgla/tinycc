.syntax unified
.thumb

.global _start 
_start:

.global test_ldrsb
test_ldrsb:
    ldrsb r1, [r0, r3]
    ldrsb r2, [r0, r6]
    ldrsb r3, [r5, r10]
    ldrsb r10, [r5, r1]
    ldrsb r4, [r10, r2]
    
    ite eq
    ldrsbeq r1, [r0, r2] 
    ldrsbne r2, [r0, r3]

    ldrsb r1, [sp, r2]
    ldrsb r2, [sp, r10]
    ldrsb r10, [sp, r1]

    ldrsb.w r1, [r0, r2]
    ldrsb r1, [r2, r4, lsl #1]

