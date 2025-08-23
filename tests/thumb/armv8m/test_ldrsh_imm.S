.syntax unified
.thumb

.global _start 
_start:

.global test_ldrsh
test_ldrsh:
    ldrsh r1, [r0, #16]
    ldrsh r2, [r0, #15]
    ldrsh r3, [r5, #10]
    ldrsh r2, [r1, #12]
    ldrsh r2, [r3]

    ittee eq
    ldrsheq r1, [r0, #110]
    ldrsheq r2, [r0, #15]
    ldrshne r3, [r5, #10]
    ldrshne r2, [r1, #12]

    ldrsh r1, [sp]
    ldrsh r2, [sp, #4]
    ldrsh r3, [sp, #4]

    itte gt  
    ldrshgt r1, [sp]
    ldrshgt r2, [sp, #4]
    ldrshle r3, [sp, #4]
   
    ldrsh.w r1, [r0, #110]
    ldrsh.w r2, [r0, #15]
    ldrsh.w r3, [r5, #10]
    ldrsh.w r2, [r1, #12]
    ldrsh.w r2, [r3]
   
    ldrsh.w r1, [sp]
    ldrsh.w r2, [sp, #4]
    ldrsh.w r3, [sp, #4]
    
    ldrsh r1, [r10, #110]
    ldrsh r12, [r0, #15]
    ldrsh r3, [r5, #10]
    ldrsh r11, [r10, #12]
    ldrsh r12, [r11]
 
    ittee eq
    ldrsheq r10, [r10, #110]
    ldrsheq r9, [r0, #15]
    ldrshne r11, [r5, #10]
    ldrshne r9, [r9, #12]
 
    ldrsh r10, [sp]
    ldrsh r12, [sp, #4]
    ldrsh r12, [sp, #-4]
    
 
    ldrsh r1, [r1, #-110]
    ldrsh r12, [r0, #-15]
    ldrsh r1, [r2], #10
    ldrsh r1, [r2], #-15
    ldrsh r1, [r3, #-110]!
    ldrsh r12, [r0, #-15]!  

