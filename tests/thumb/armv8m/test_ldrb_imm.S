.syntax unified
.thumb

.global _start 
_start:

.global test_ldrb
test_ldrb:
    ldrb r1, [r0, #16]
    ldrb r2, [r0, #15]
    ldrb r3, [r5, #10]
    ldrb r2, [r1, #12]
    ldrb r2, [r3]

    ittee eq
    ldrbeq r1, [r0, #110]
    ldrbeq r2, [r0, #15]
    ldrbne r3, [r5, #10]
    ldrbne r2, [r1, #12]

    ldrb r1, [sp]
    ldrb r2, [sp, #4]
    ldrb r3, [sp, #4]

    itte gt  
    ldrbgt r1, [sp]
    ldrbgt r2, [sp, #4]
    ldrble r3, [sp, #4]
   
    ldrb.w r1, [r0, #110]
    ldrb.w r2, [r0, #15]
    ldrb.w r3, [r5, #10]
    ldrb.w r2, [r1, #12]
    ldrb.w r2, [r3]
   
    ldrb.w r1, [sp]
    ldrb.w r2, [sp, #4]
    ldrb.w r3, [sp, #4]
    
    ldrb r1, [r10, #110]
    ldrb r12, [r0, #15]
    ldrb r3, [r5, #10]
    ldrb r11, [r10, #12]
    ldrb r12, [r11]
 
    ittee eq
    ldrbeq r10, [r10, #110]
    ldrbeq r9, [r0, #15]
    ldrbne r11, [r5, #10]
    ldrbne r9, [r9, #12]
 
    ldrb r10, [sp]
    ldrb r12, [sp, #4]
    ldrb r12, [sp, #-4]
    
 
    ldrb r1, [r1, #-110]
    ldrb r12, [r0, #-15]
    ldrb r1, [r2], #10
    ldrb r1, [r2], #-15
    ldrb r1, [r3, #-110]!
    ldrb r12, [r0, #-15]!  

