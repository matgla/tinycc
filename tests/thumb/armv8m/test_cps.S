.syntax unified
.thumb

.global _start 
_start:

.global test_cps
test_cps:
    cpsid i 
    cpsie i
    cpsid f 
    cpsie f
    cpsid if
    cpsie if


   
    

