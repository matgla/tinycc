.syntax unified
.thumb

.global _start
_start:

.global test_strexh
test_strexh:
    strexh r8, r9, [r10]

    itee eq
    strexheq r8, r4, [r10]
    strexhne r1, r4, [r2]
    strexhne r8, r4, [r1]