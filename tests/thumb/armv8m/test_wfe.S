.syntax unified
.thumb

.global _start
_start:

.global test_wfe
test_wfe:
    wfe
    wfe.w
