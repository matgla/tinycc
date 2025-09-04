.syntax unified
.thumb

.global _start 
_start:

.global test_dmb
test_dsb:
    dsb
    dsb sy
