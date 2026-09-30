/*
 * Fixed-size word copies the compiler calls instead of expanding a struct
 * copy inline (arm-thumb-gen.c, thumb_copy_stub_call).
 *
 *   __tcc_wcopy_N   (r0 = dest, r1 = src), N = 3..32 words
 *
 * Both pointers are word aligned and the ranges are the same object or
 * disjoint ones (C struct assignment, a by-value argument, an initialiser
 * image), so the words go forwards, each group loaded before it is stored.
 * Clobbers r0-r3, ip and lr -- what the inline copy clobbered -- and nothing
 * else: no stack, no R9, no flags.
 *
 * Three chains, one per N mod 3.  Each entry is the point in its chain with
 * N/3 LDM/STM pairs of three words still to go; the chain ends in the 0-2
 * word remainder and the return.  An N-register LDM/STM takes 1+N cycles on
 * the M33, so three-register pairs cost what the inline four-register ones
 * did per word; the call adds the BL and the BX.  An inline 64-byte copy was
 * 32 bytes of code at every site; the call is 4.
 */

.syntax unified
.thumb

.text
.align 2

/* N = 3k + 0 */
.global __tcc_wcopy_30
.type __tcc_wcopy_30, %function
.thumb_func
__tcc_wcopy_30:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_27
.type __tcc_wcopy_27, %function
.thumb_func
__tcc_wcopy_27:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_24
.type __tcc_wcopy_24, %function
.thumb_func
__tcc_wcopy_24:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_21
.type __tcc_wcopy_21, %function
.thumb_func
__tcc_wcopy_21:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_18
.type __tcc_wcopy_18, %function
.thumb_func
__tcc_wcopy_18:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_15
.type __tcc_wcopy_15, %function
.thumb_func
__tcc_wcopy_15:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_12
.type __tcc_wcopy_12, %function
.thumb_func
__tcc_wcopy_12:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_9
.type __tcc_wcopy_9, %function
.thumb_func
__tcc_wcopy_9:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_6
.type __tcc_wcopy_6, %function
.thumb_func
__tcc_wcopy_6:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_3
.type __tcc_wcopy_3, %function
.thumb_func
__tcc_wcopy_3:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
    bx      lr

/* N = 3k + 1 */
.global __tcc_wcopy_31
.type __tcc_wcopy_31, %function
.thumb_func
__tcc_wcopy_31:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_28
.type __tcc_wcopy_28, %function
.thumb_func
__tcc_wcopy_28:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_25
.type __tcc_wcopy_25, %function
.thumb_func
__tcc_wcopy_25:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_22
.type __tcc_wcopy_22, %function
.thumb_func
__tcc_wcopy_22:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_19
.type __tcc_wcopy_19, %function
.thumb_func
__tcc_wcopy_19:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_16
.type __tcc_wcopy_16, %function
.thumb_func
__tcc_wcopy_16:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_13
.type __tcc_wcopy_13, %function
.thumb_func
__tcc_wcopy_13:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_10
.type __tcc_wcopy_10, %function
.thumb_func
__tcc_wcopy_10:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_7
.type __tcc_wcopy_7, %function
.thumb_func
__tcc_wcopy_7:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_4
.type __tcc_wcopy_4, %function
.thumb_func
__tcc_wcopy_4:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
    ldr     r2, [r1]
    str     r2, [r0]
    bx      lr

/* N = 3k + 2 */
.global __tcc_wcopy_32
.type __tcc_wcopy_32, %function
.thumb_func
__tcc_wcopy_32:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_29
.type __tcc_wcopy_29, %function
.thumb_func
__tcc_wcopy_29:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_26
.type __tcc_wcopy_26, %function
.thumb_func
__tcc_wcopy_26:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_23
.type __tcc_wcopy_23, %function
.thumb_func
__tcc_wcopy_23:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_20
.type __tcc_wcopy_20, %function
.thumb_func
__tcc_wcopy_20:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_17
.type __tcc_wcopy_17, %function
.thumb_func
__tcc_wcopy_17:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_14
.type __tcc_wcopy_14, %function
.thumb_func
__tcc_wcopy_14:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_11
.type __tcc_wcopy_11, %function
.thumb_func
__tcc_wcopy_11:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_8
.type __tcc_wcopy_8, %function
.thumb_func
__tcc_wcopy_8:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
.global __tcc_wcopy_5
.type __tcc_wcopy_5, %function
.thumb_func
__tcc_wcopy_5:
    ldmia   r1!, {r2, r3, ip}
    stmia   r0!, {r2, r3, ip}
    ldrd    r2, r3, [r1]
    strd    r2, r3, [r0]
    bx      lr
