.syntax unified
.thumb

.global _start 
_start:

.global test_isb
test_isb:
    isb
    isb sy
