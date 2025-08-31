.syntax unified
.thumb

.global _start 
_start:

.global test_pop
test_pop:
    pop {r0, r1, r2, r4, r5, r6, r7}
    pop {r0, r2, r4, r7}
    pop {r0-r8}
    pop {r2}
    pop {pc}