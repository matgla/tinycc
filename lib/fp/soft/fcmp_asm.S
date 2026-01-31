/*
 * Soft-float Comparison Assembly Helpers
 * Implements ARM EABI comparison functions that set CPSR flags
 *
 * These functions are called by the compiler's code generator for
 * floating-point comparisons. They set the CPSR flags so that
 * subsequent conditional branches work correctly.
 *
 * Functions:
 *   __aeabi_cfcmple - Compare a <= b, set CPSR flags
 *   __aeabi_cfrcmple - Compare b <= a (reversed), set CPSR flags
 *   __aeabi_cfcmplt - Compare a < b, set CPSR flags
 *   __aeabi_cfcmpge - Compare a >= b, set CPSR flags
 *   __aeabi_cfcmpgt - Compare a > b, set CPSR flags
 *   __aeabi_cfcmpeq - Compare a == b, set CPSR flags
 *
 * ARM EABI flag conventions for cfcmple:
 *   Z=1 if equal, C=0 if less than (a < b)
 *   For a <= b: Z || !C should be true
 */

.syntax unified
.thumb

.text
.align 2

/*
 * __aeabi_cfcmple: Compare floats a <= b, set CPSR flags
 * Args: r0 = a, r1 = b
 * Clobbers: r0-r3, r12 (per AAPCS)
 *
 * To set flags properly:
 *   If a <= b: we need Z=1 or C=0
 *   We call __aeabi_fcmple which returns 1 if true, 0 if false
 *   Then we do cmp r0, #1 to set flags
 */
.global __aeabi_cfcmple
.type __aeabi_cfcmple, %function
__aeabi_cfcmple:
    push {lr}
    bl __aeabi_fcmple    /* r0 = (a <= b) ? 1 : 0 */
    cmp r0, #1           /* Set flags: Z=1 if r0==1 (a<=b), Z=0 otherwise */
    pop {pc}

/*
 * __aeabi_cfrcmple: Compare floats b <= a (reversed), set CPSR flags
 * Args: r0 = a, r1 = b (call as b <= a)
 */
.global __aeabi_cfrcmple
.type __aeabi_cfrcmple, %function
__aeabi_cfrcmple:
    push {lr}
    mov r2, r0           /* Swap arguments: r0 <-> r1 */
    mov r0, r1
    mov r1, r2
    bl __aeabi_fcmple    /* r0 = (b <= a) ? 1 : 0 */
    cmp r0, #1
    pop {pc}

/*
 * __aeabi_cfcmplt: Compare floats a < b, set CPSR flags
 */
.global __aeabi_cfcmplt
.type __aeabi_cfcmplt, %function
__aeabi_cfcmplt:
    push {lr}
    bl __aeabi_fcmplt    /* r0 = (a < b) ? 1 : 0 */
    cmp r0, #1
    pop {pc}

/*
 * __aeabi_cfcmpge: Compare floats a >= b, set CPSR flags
 */
.global __aeabi_cfcmpge
.type __aeabi_cfcmpge, %function
__aeabi_cfcmpge:
    push {lr}
    bl __aeabi_fcmpge    /* r0 = (a >= b) ? 1 : 0 */
    cmp r0, #1
    pop {pc}

/*
 * __aeabi_cfcmpgt: Compare floats a > b, set CPSR flags
 */
.global __aeabi_cfcmpgt
.type __aeabi_cfcmpgt, %function
__aeabi_cfcmpgt:
    push {lr}
    bl __aeabi_fcmpgt    /* r0 = (a > b) ? 1 : 0 */
    cmp r0, #1
    pop {pc}

/*
 * __aeabi_cfcmpeq: Compare floats a == b, set CPSR flags
 */
.global __aeabi_cfcmpeq
.type __aeabi_cfcmpeq, %function
__aeabi_cfcmpeq:
    push {lr}
    bl __aeabi_fcmpeq    /* r0 = (a == b) ? 1 : 0 */
    cmp r0, #1           /* Z=1 if equal, Z=0 if not equal */
    pop {pc}
