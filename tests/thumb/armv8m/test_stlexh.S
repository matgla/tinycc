.syntax unified
.thumb

.global _start
_start:

.global test_stlexh
test_stlexh:
    stlexh r8, r2, [r10]
    stlexh r1, r2, [r2]
    stlexh r8, r2, [r1]

    itee eq
    stlexheq r8, r10, [r10]
    stlexhne r1, r3, [r2]
    stlexhne r8, r2, [r1]