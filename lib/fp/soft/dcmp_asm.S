/*
 * Soft-float Double Comparison Assembly Helpers
 * Implements ARM EABI comparison functions that set CPSR flags
 *
 * These functions call dcmp_core() which returns:
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
 * __aeabi_cdcmple: Compare doubles, set CPSR flags
 * Args: R0:R1 = a (double), R2:R3 = b (double)
 */
.global __aeabi_cdcmple
.type __aeabi_cdcmple, %function
__aeabi_cdcmple:
    push {lr}
    bl dcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cdcmple, .-__aeabi_cdcmple

/*
 * __aeabi_cdrcmple: Compare doubles reversed (b <= a), set CPSR flags
 * Args: R0:R1 = a, R2:R3 = b → calls dcmp_core(b, a)
 */
.global __aeabi_cdrcmple
.type __aeabi_cdrcmple, %function
__aeabi_cdrcmple:
    push {r4, lr}
    mov r4, r0
    mov r0, r2
    mov r2, r4
    mov r4, r1
    mov r1, r3
    mov r3, r4
    bl dcmp_core
    cmp r0, #0
    pop {r4, pc}
.size __aeabi_cdrcmple, .-__aeabi_cdrcmple

/*
 * __aeabi_cdcmplt: Compare doubles (a < b), set CPSR flags
 */
.global __aeabi_cdcmplt
.type __aeabi_cdcmplt, %function
__aeabi_cdcmplt:
    push {lr}
    bl dcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cdcmplt, .-__aeabi_cdcmplt

/*
 * __aeabi_cdcmpeq: Compare doubles (a == b), set CPSR flags
 */
.global __aeabi_cdcmpeq
.type __aeabi_cdcmpeq, %function
__aeabi_cdcmpeq:
    push {lr}
    bl dcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cdcmpeq, .-__aeabi_cdcmpeq

/*
 * __aeabi_cdcmpgt: Compare doubles (a > b), set CPSR flags
 */
.global __aeabi_cdcmpgt
.type __aeabi_cdcmpgt, %function
__aeabi_cdcmpgt:
    push {lr}
    bl dcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cdcmpgt, .-__aeabi_cdcmpgt

/*
 * __aeabi_cdcmpge: Compare doubles (a >= b), set CPSR flags
 */
.global __aeabi_cdcmpge
.type __aeabi_cdcmpge, %function
__aeabi_cdcmpge:
    push {lr}
    bl dcmp_core
    cmp r0, #0
    pop {pc}
.size __aeabi_cdcmpge, .-__aeabi_cdcmpge
