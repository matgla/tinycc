.syntax unified
.thumb

.global _start 
_start:

.global test_csdb
test_csdb:
    csdb
    csdb.w
