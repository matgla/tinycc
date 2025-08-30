.syntax unified
.thumb

.global _start 
_start:

.global test_ldrsht
test_ldrsht:
    ldrsht r1, [r2, #32]
    ldrsht r10, [r2]
   
    ite eq 
    ldrshteq r1, [r2, #32]
    ldrshtne r10, [r2]
    