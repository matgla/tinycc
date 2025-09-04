.syntax unified
.thumb

.global _start 
_start:

.global test_mls
test_mls:
    mls r2, r3, r8, r10 