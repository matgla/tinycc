.syntax unified
.thumb

.global _start 
_start:

.global test_mla
test_mla:
    mla r2, r3, r8, r10 