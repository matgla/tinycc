.syntax unified
.thumb

.global _start 
_start:

.global test_dmb
test_dmb:
    dmb
    dmb sy
