.syntax unified
.thumb

.global _start 
_start:

.global test_ldrexb
test_ldrexb:
    ldrexb r8, [r10]

    itee eq 
    ldrexbeq r8, [r10]
    ldrexbne r1, [r2]
    ldrexbne r8, [r1] 