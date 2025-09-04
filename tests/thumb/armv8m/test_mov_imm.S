.syntax unified
.thumb

.global test_mov_imm
test_mov_imm:
// T1: movs Rd, #imm8 (16-bit)
// Only low registers (r0-r7)
movs r0, #1
movs r7, #255
movs.w r2, #1
movs.n r1, #2

// T3: mov Rd, #imm16 (32-bit, MOVW)
// Any register (r0-r15)
movw r8, #0x1234
mov r9, #1
mov r0, #0xfff
mov r1, #10

cmp r0, #1
ittte eq
moveq r8, #0xdead
moveq r1, #0x0d
moveq r10, #0x0f
movne r3, #0x12

ittee cs
movcs r2, #0xde
movcs r8, #0xab12
movcc r9, #0x12
movcc r10, #0xffff

iteet mi
movmi r2, #0xde
movpl r8, #0xab12
movpl r9, #0x12
movmi r10, #0xffff

it ne
movne r0, #1

ite gt
movgt r1, #2
movle r1, #4

it eq
moveq r1, #1

itt eq
moveq r2, #2 
moveq r3, #3

ite eq
moveq r4, #3
movne r5, #5

ittt eq
moveq r5, #7
moveq r8, #9
moveq r9, #10

itte eq
moveq r9, #100
moveq r10, #123
movne r11, #10

itet eq
moveq r9, #100
movne r10, #123
moveq r11, #10

itee eq
moveq r9, #100
movne r10, #123
movne r11, #10

itttt eq 
moveq r9, #100
moveq r10, #123
moveq r11, #10
moveq r12, #120

ittte eq 
moveq r9, #100
moveq r10, #123
moveq r11, #10
movne r12, #120

ittet eq 
moveq r9, #100
moveq r10, #123
movne r11, #10
moveq r12, #120

ittee eq 
moveq r9, #100
moveq r10, #123
movne r11, #10
movne r12, #120

itett eq 
moveq r9, #100
movne r10, #123
moveq r11, #10
moveq r12, #120

itete eq 
moveq r9, #100
movne r10, #123
moveq r11, #10
movne r12, #120

iteet eq 
moveq r9, #100
movne r10, #123
movne r11, #10
moveq r12, #120

iteee eq 
moveq r9, #100
movne r10, #123
movne r11, #10
movne r12, #120
