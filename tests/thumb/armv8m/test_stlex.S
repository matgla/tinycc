.syntax unified
.thumb

.global _start
_start:

.global test_stlex
test_stlex:
    stlex r8, r2, [r10]
    stlex r1, r2, [r2]
    stlex r8, r2, [r1]

    itee eq
    stlexeq r8, r10, [r10]
    stlexne r1, r3, [r2]
    stlexne r8, r2, [r1]