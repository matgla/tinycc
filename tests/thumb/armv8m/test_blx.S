.syntax unified
.thumb

.global _start 
_start:

.global test_blx
test_blx:
    blx r0 
    blx r1 
    blx r2 
    blx r3
    blx r4 
    blx r5 
    blx r6 
    blx r7 
    blx r8 
    blx r9 
    blx r10 
    blx r11 
    blx r12 
    blx ip 
    blx lr 

    itte eq 
    moveq r1, #0xab
    moveq r2, #0xcd 
    blxne r3
