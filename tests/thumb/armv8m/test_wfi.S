.syntax unified
.thumb

.global _start
_start:

.global test_wfi
test_wfi:
    wfi
    wfi.w
