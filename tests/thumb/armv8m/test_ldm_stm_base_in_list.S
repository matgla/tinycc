.syntax unified
.thumb

.global _start
_start:

.global test_ldm_stm_base_in_list
test_ldm_stm_base_in_list:
    ldm   r0, {r0, r1}
    ldm   r1, {r0, r1, r2}
    ldm.w r0, {r0, r1}
    ldm   r8, {r0, r8}
    ldm   r0!, {r1, r2}
    ldm.w r0!, {r1, r2}
    stm   r0!, {r0, r1}
    stm   r1, {r0, r1}
    stm.w r1, {r0, r1}
    stm   r1!, {r1, r2}
    stm.w r1!, {r0, r2}
    stm   r8, {r0, r8}
    bx lr
