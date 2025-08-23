.syntax unified
.thumb

.global _start 
_start:

.global test_ldrb
test_ldrb:
    ldrb r1, [r0, r3]
    ldrb r2, [r0, r6]
    ldrb r3, [r5, r10]
    ldrb r10, [r5, r1]
    ldrb r4, [r10, r2]
    
    ite eq
    ldrbeq r1, [r0, r2] 
    ldrbne r2, [r0, r3]

    ldrb r1, [sp, r2]
    ldrb r2, [sp, r10]
    ldrb r10, [sp, r1]

    ldrb.w r1, [r0, r2]
    ldrb r1, [r2, r4, lsl #1]

