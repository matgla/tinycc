.syntax unified
.thumb

.global _start 
_start:

.global test_ldrsb
test_ldrsb:
    ldrsb r1, [r0, #16]
    ldrsb r2, [r0, #15]
    ldrsb r3, [r5, #10]
    ldrsb r2, [r1, #12]
    ldrsb r2, [r3]

    ittee eq
    ldrsbeq r1, [r0, #110]
    ldrsbeq r2, [r0, #15]
    ldrsbne r3, [r5, #10]
    ldrsbne r2, [r1, #12]

    ldrsb r1, [sp]
    ldrsb r2, [sp, #4]
    ldrsb r3, [sp, #4]

    itte gt  
    ldrsbgt r1, [sp]
    ldrsbgt r2, [sp, #4]
    ldrsble r3, [sp, #4]
   
    ldrsb.w r1, [r0, #110]
    ldrsb.w r2, [r0, #15]
    ldrsb.w r3, [r5, #10]
    ldrsb.w r2, [r1, #12]
    ldrsb.w r2, [r3]
   
    ldrsb.w r1, [sp]
    ldrsb.w r2, [sp, #4]
    ldrsb.w r3, [sp, #4]
    
    ldrsb r1, [r10, #110]
    ldrsb r12, [r0, #15]
    ldrsb r3, [r5, #10]
    ldrsb r11, [r10, #12]
    ldrsb r12, [r11]
 
    ittee eq
    ldrsbeq r10, [r10, #110]
    ldrsbeq r9, [r0, #15]
    ldrsbne r11, [r5, #10]
    ldrsbne r9, [r9, #12]
 
    ldrsb r10, [sp]
    ldrsb r12, [sp, #4]
    ldrsb r12, [sp, #-4]
    
 
    ldrsb r1, [r1, #-110]
    ldrsb r12, [r0, #-15]
    ldrsb r1, [r2], #10
    ldrsb r1, [r2], #-15
    ldrsb r1, [r3, #-110]!
    ldrsb r12, [r0, #-15]!  

