.syntax unified
.thumb

.global _start
_start:

.global test_strexb
test_strexb:
    strexb r8, r9, [r10]

    itee eq
    strexbeq r8, r4, [r10]
    strexbne r1, r4, [r2]
    strexbne r8, r4, [r1]