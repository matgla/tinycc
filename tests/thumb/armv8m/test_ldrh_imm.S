.syntax unified
.thumb

.global _start 
_start:

.global test_ldrh
test_ldrh:
    ldrh r1, [r0, #16]
    ldrh r2, [r0, #15]
    ldrh r3, [r5, #10]
    ldrh r2, [r1, #12]
    ldrh r2, [r3]

    ittee eq
    ldrheq r1, [r0, #110]
    ldrheq r2, [r0, #15]
    ldrhne r3, [r5, #10]
    ldrhne r2, [r1, #12]

    ldrh r1, [sp]
    ldrh r2, [sp, #4]
    ldrh r3, [sp, #4]

    itte gt  
    ldrhgt r1, [sp]
    ldrhgt r2, [sp, #4]
    ldrhle r3, [sp, #4]
   
    ldrh.w r1, [r0, #110]
    ldrh.w r2, [r0, #15]
    ldrh.w r3, [r5, #10]
    ldrh.w r2, [r1, #12]
    ldrh.w r2, [r3]
   
    ldrh.w r1, [sp]
    ldrh.w r2, [sp, #4]
    ldrh.w r3, [sp, #4]
    
    ldrh r1, [r10, #110]
    ldrh r12, [r0, #15]
    ldrh r3, [r5, #10]
    ldrh r11, [r10, #12]
    ldrh r12, [r11]
 
    ittee eq
    ldrheq r10, [r10, #110]
    ldrheq r9, [r0, #15]
    ldrhne r11, [r5, #10]
    ldrhne r9, [r9, #12]
 
    ldrh r10, [sp]
    ldrh r12, [sp, #4]
    ldrh r12, [sp, #-4]
    
 
    ldrh r1, [r1, #-110]
    ldrh r12, [r0, #-15]
    ldrh r1, [r2], #10
    ldrh r1, [r2], #-15
    ldrh r1, [r3, #-110]!
    ldrh r12, [r0, #-15]!  

