.syntax unified
.thumb

.global _start 
_start:

.global test_ldr_literal
test_ldr_literal:
    ldr r1, next_label 
    ldr r10, next_label 
    
next_label: 
    ldr r0, test_ldr_literal
    ldr r2, long_literal
    ldr r3, far_symbol
    ldr r10, test_ldr_literal
    ldr r11, long_literal
    ldr r12, far_symbol


.asciz "asfoasheafeuafuhsefuahfhaofhiefihasfhueshoupah"

long_literal:
    ldr r0, test_ldr_literal 
    ldr r10, test_ldr_literal 

.global far_symbol
far_symbol:
    ldr r1, long_literal
    ldr r12, long_literal
    
