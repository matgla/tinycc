.syntax unified
.thumb

.global _start 
_start:

.global test_ldrd_literal
test_ldrd_literal:
    ldrd r1, r2, next_label 
    ldrd r10, r11, next_label 
    
next_label: 
    movs r1, #0 
    ldrd r0, r2, test_ldrd_literal
    ldrd r2, r3, long_literal
    ldrd r3, r4, far_symbol
    ldrd r10, r1, test_ldrd_literal
    ldrd r11, r1, long_literal
    ldrd r12, r1, far_symbol


.asciz "asfoasheafeuafuhsefuahfhaofhiefihasfhueshupah"
long_literal:
    ldrd r0, r1, test_ldrd_literal 
    ldrd r10, r11, test_ldrd_literal 

.global far_symbol
far_symbol:
    ldrd r1, r2, long_literal
    ldrd r12, r3, long_literal
    
