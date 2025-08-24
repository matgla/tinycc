.syntax unified
.thumb

.global _start 
_start:

.global test_it
test_it:
it eq
moveq r0, #0

itt eq
moveq r0, #0
moveq r0, #0

ite eq
moveq r0, #0
movne r0, #0

ittt eq
moveq r0, #0
moveq r0, #0
moveq r0, #0

itte eq
moveq r0, #0
moveq r0, #0
movne r0, #0

itte lt 
movlt r0, #0
movlt r0, #0
movge r0, #0

itet eq
moveq r0, #0
movne r0, #0
moveq r0, #0

itee eq
moveq r0, #0
movne r0, #0
movne r0, #0

itttt eq
moveq r0, #0
moveq r0, #0
moveq r0, #0
moveq r0, #0

ittte eq
moveq r0, #0
moveq r0, #0
moveq r0, #0
movne r0, #0

ittet eq
moveq r0, #0
moveq r0, #0
movne r0, #0
moveq r0, #0

ittee eq
moveq r0, #0
moveq r0, #0
movne r0, #0
movne r0, #0

itett eq
moveq r0, #0
movne r0, #0
moveq r0, #0
moveq r0, #0

itete eq
moveq r0, #0
movne r0, #0
moveq r0, #0
movne r0, #0

iteet eq
moveq r0, #0
movne r0, #0
movne r0, #0
moveq r0, #0

iteee eq
moveq r0, #0
movne r0, #0
movne r0, #0
movne r0, #0

