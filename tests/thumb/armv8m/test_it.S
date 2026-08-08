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


/* Every condition mnemonic after the IT, including the hs/lo aliases for
 * cs/cc.  The alias pair is the regression: the IT parser had its own
 * hand-rolled condition table that knew cs and cc but not hs and lo, and fell
 * through to AL for anything it did not recognize, so "ite hs" assembled as
 * "ite al" -- whose else-branch condition is UNPREDICTABLE.  Encodings here
 * must match gcc's. */
.global test_it_conditions
test_it_conditions:
it eq
moveq r0, #0
it ne
movne r0, #0
it cs
movcs r0, #0
it hs
movhs r0, #0
it cc
movcc r0, #0
it lo
movlo r0, #0
it mi
movmi r0, #0
it pl
movpl r0, #0
it vs
movvs r0, #0
it vc
movvc r0, #0
it hi
movhi r0, #0
it ls
movls r0, #0
it ge
movge r0, #0
it lt
movlt r0, #0
it gt
movgt r0, #0
it le
movle r0, #0

ite hs
movhs r0, #1
movlo r0, #0

ite lo
movlo r0, #1
movhs r0, #0

itte hs
movhs r0, #1
movhs r0, #2
movlo r0, #0
