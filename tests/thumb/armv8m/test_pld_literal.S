.syntax unified
.thumb

.global _start 
_start:

.global test_pld_literal
test_pld_literal:
    pld next_label 
    pld next_label 
    
next_label: 
    pld test_pld_literal
    pld long_literal
    pld far_symbol
    pld test_pld_literal
    pld long_literal
    pld far_symbol


.asciz "asfoasheafeuafuhsefuahfhaofhiefihasfhueshoupah"

long_literal:
    pld test_pld_literal 
    pld test_pld_literal 

.global far_symbol
far_symbol:
    pld long_literal
    pld long_literal

    
