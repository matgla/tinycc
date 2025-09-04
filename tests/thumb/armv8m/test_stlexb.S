.syntax unified
.thumb

.global _start
_start:

.global test_stlexb
test_stlexb:
    stlexb r8, r2, [r10]
    stlexb r1, r2, [r2]
    stlexb r8, r2, [r1]

    itee eq
    stlexbeq r8, r10, [r10]
    stlexbne r1, r3, [r2]
    stlexbne r8, r2, [r1]