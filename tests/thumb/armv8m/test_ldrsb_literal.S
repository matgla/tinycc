.syntax unified
.thumb

.global _start 
_start:

.global test_ldrsb_literal
test_ldrsb_literal:
    ldrsb r1, next_label 
    ldrsb r10, next_label 
    
next_label: 
    ldrsb r0, test_ldrsb_literal
    ldrsb r2, long_literal
    ldrsb r3, far_symbol
    ldrsb r10, test_ldrsb_literal
    ldrsb r11, long_literal
    ldrsb r12, far_symbol


.asciz "asfoasheafeuafuhsefuahfhaofhiefihasfhueshoupah"

long_literal:
    ldrsb r0, test_ldrsb_literal 
    ldrsb r10, test_ldrsb_literal 

.global far_symbol
far_symbol:
    ldrsb r1, long_literal
    ldrsb r12, long_literal
    
