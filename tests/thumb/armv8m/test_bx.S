.syntax unified
.thumb

.global test_bx
test_bx:
    bx r0
    bx r1 
    bx r2 
    bx r3 
    bx r4
    bx r5
    bx r6 
    bx r7
    bx r8
    bx r9 
    bx r10 
    bx r11 
    bx r12 
    bx lr 
    bx pc
    bx ip

    bx.n r0 
    
    itte gt 
    movgt r0, #1
    movgt r2, #2
    bxle r10
