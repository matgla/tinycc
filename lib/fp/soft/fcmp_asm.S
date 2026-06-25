/*
 * Soft-float Single-Precision Comparison Assembly Helpers
 * Implements ARM EABI comparison functions that set CPSR flags
 *
 * These functions call fcmp_core() which returns:
 *   -1 if a < b
 *    0 if a == b
 *    1 if a > b
 *    2 if unordered (NaN)
 *
 * Then CMP r0, #0 sets APSR flags so that standard signed
 * condition codes (beq, bne, blt, ble, bgt, bge) work correctly:
 *   r=-1: N=1, Z=0 → blt true, beq false
 *   r= 0: N=0, Z=1 → beq true, bge true, ble true
 *   r= 1: N=0, Z=0 → bgt true, bne true
 */

.syntax unified
.thumb

.text
.align 2

/*
 * __aeabi_cfcmple: Compare floats a <= b, set CPSR flags
 * Args: r0 = a, r1 = b
 */
.global __aeabi_cfcmple
.type __aeabi_cfcmple, %function
__aeabi_cfcmple:
    push {lr}
    bl fcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cfcmple, .-__aeabi_cfcmple

/*
 * __aeabi_cfrcmple: Compare floats b <= a (reversed), set CPSR flags
 * Args: r0 = a, r1 = b → calls fcmp_core(b, a)
 */
.global __aeabi_cfrcmple
.type __aeabi_cfrcmple, %function
__aeabi_cfrcmple:
    push {lr}
    mov r2, r0
    mov r0, r1
    mov r1, r2
    bl fcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cfrcmple, .-__aeabi_cfrcmple

/*
 * __aeabi_cfcmplt: Compare floats a < b, set CPSR flags
 */
.global __aeabi_cfcmplt
.type __aeabi_cfcmplt, %function
__aeabi_cfcmplt:
    push {lr}
    bl fcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cfcmplt, .-__aeabi_cfcmplt

/*
 * __aeabi_cfcmpge: Compare floats a >= b, set CPSR flags
 */
.global __aeabi_cfcmpge
.type __aeabi_cfcmpge, %function
__aeabi_cfcmpge:
    push {lr}
    bl fcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cfcmpge, .-__aeabi_cfcmpge

/*
 * __aeabi_cfcmpgt: Compare floats a > b, set CPSR flags
 */
.global __aeabi_cfcmpgt
.type __aeabi_cfcmpgt, %function
__aeabi_cfcmpgt:
    push {lr}
    bl fcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cfcmpgt, .-__aeabi_cfcmpgt

/*
 * __aeabi_cfcmpeq: Compare floats a == b, set CPSR flags
 */
.global __aeabi_cfcmpeq
.type __aeabi_cfcmpeq, %function
__aeabi_cfcmpeq:
    push {lr}
    bl fcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cfcmpeq, .-__aeabi_cfcmpeq
