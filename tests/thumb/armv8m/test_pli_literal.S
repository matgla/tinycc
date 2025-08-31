.syntax unified
.thumb

.global _start 
_start:

.global test_pli_literal
test_pli_literal:
    pli next_label 
    pli next_label 
    
next_label: 
    pli test_pli_literal
    pli long_literal
    pli far_symbol
    pli test_pli_literal
    pli long_literal
    pli far_symbol


.asciz "asfoasheafeuafuhsefuahfhaofhiefihasfhueshoupah"

long_literal:
    pli test_pli_literal 
    pli test_pli_literal 

.global far_symbol
far_symbol:
    pli long_literal
    pli long_literal
