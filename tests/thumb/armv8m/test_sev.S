.syntax unified
.thumb

.global _start
_start:

.global test_sev
test_sev:
    sev
    sev.w
