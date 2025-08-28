.syntax unified
.thumb

.global _start 
_start:

.global test_lda 
test_lda:
    lda r1, [r2]
    lda r9, [r10] 
    lda r1, [r11] 

    ite eq
    ldaeq r2, [r3]
    ldane r10, [r12]