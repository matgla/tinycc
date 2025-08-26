.syntax unified
.thumb

.global _start 
_start:

.global test_bkpt
test_bkpt:
    bkpt 1
    bkpt #4

