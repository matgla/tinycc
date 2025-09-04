.syntax unified
.thumb

.global _start 
_start:

.global test_ldrsh_literal
test_ldrsh_literal:
    ldrsh r1, next_label 
    ldrsh r10, next_label 
    
next_label: 
    ldrsh r0, test_ldrsh_literal
    ldrsh r2, long_literal
    ldrsh r3, far_symbol
    ldrsh r10, test_ldrsh_literal
    ldrsh r11, long_literal
    ldrsh r12, far_symbol


.asciz "asfoasheafeuafuhsefuahfhaofhiefihasfhueshoupah"

long_literal:
    ldrsh r0, test_ldrsh_literal 
    ldrsh r10, test_ldrsh_literal 

.global far_symbol
far_symbol:
    ldrsh r1, long_literal
    ldrsh r12, long_literal
    
