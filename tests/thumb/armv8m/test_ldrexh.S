.syntax unified
.thumb

.global _start 
_start:

.global test_ldrexh
test_ldrexh:
    ldrexh r8, [r10]

    itee eq 
    ldrexheq r8, [r10]
    ldrexhne r1, [r2]
    ldrexhne r8, [r1] 