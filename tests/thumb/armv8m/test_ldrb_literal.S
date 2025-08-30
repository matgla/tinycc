.syntax unified
.thumb

.global _start 
_start:

.global test_ldrb_literal
test_ldrb_literal:
    ldrb r1, next_label 
    ldrb r10, next_label 
    
next_label: 
    ldrb r0, test_ldrb_literal
    ldrb r2, long_literal
    ldrb r3, far_symbol
    ldrb r10, test_ldrb_literal
    ldrb r11, long_literal
    ldrb r12, far_symbol


.asciz "asfoasheafeuafuhsefuahfhaofhiefihasfhueshoupah"

long_literal:
    ldrb r0, test_ldrb_literal 
    ldrb r10, test_ldrb_literal 

.global far_symbol
far_symbol:
    ldrb r1, long_literal
    ldrb r12, long_literal
    
