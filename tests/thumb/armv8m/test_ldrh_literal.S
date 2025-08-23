.syntax unified
.thumb

.global _start 
_start:

.global test_ldrh_literal
test_ldrh_literal:
    ldrh r1, next_label 
    ldrh r10, next_label 
    
next_label: 
    ldrh r0, test_ldrh_literal
    ldrh r2, long_literal
    ldrh r3, far_symbol
    ldrh r10, test_ldrh_literal
    ldrh r11, long_literal
    ldrh r12, far_symbol


.asciz "asfoasheafeuafuhsefuahfhaofhiefihasfhueshoupah"

long_literal:
    ldrh r0, test_ldrh_literal 
    ldrh r10, test_ldrh_literal 

.global far_symbol
far_symbol:
    ldrh r1, long_literal
    ldrh r12, long_literal
    
