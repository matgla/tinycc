/* ARM EABI 64-bit div/mod wrappers.
 *
 * EABI requires:
 *   __aeabi_uldivmod / __aeabi_ldivmod
 *     - args in r0:r1 (numerator), r2:r3 (denominator)
 *     - returns quotient in r0:r1 and remainder in r2:r3
 *
 * TinyCC ARM/Thumb currently miscompiles C functions returning a 16-byte struct
 * (using an implicit sret pointer), which breaks the required ABI here.
 *
 * Implement the public symbols in assembly and delegate the computation to C
 * helpers that write results to memory.
 */

.syntax unified
#ifdef __thumb__
.thumb
#endif

.text
.align 2

#ifdef __thumb__
.thumb_func
#endif
.global __aeabi_uldivmod
.type __aeabi_uldivmod, %function
__aeabi_uldivmod:
    push    {r4, r5, r6, lr}
    sub     sp, sp, #16          /* output slots: qlo,qhi,rlo,rhi */
    mov     r6, sp               /* r6 = &out[0] */

    /* Stack args for helper: (q_lo,q_hi,r_lo,r_hi) pointers */
    add     r4, r6, #12          /* rhi */
    push    {r4}
    add     r4, r6, #8           /* rlo */
    push    {r4}
    add     r4, r6, #4           /* qhi */
    push    {r4}
    mov     r4, r6               /* qlo */
    push    {r4}

    bl      __tcc_aeabi_uldivmod_helper

    add     sp, sp, #16          /* pop pointer args */

    ldr     r0, [r6, #0]
    ldr     r1, [r6, #4]
    ldr     r2, [r6, #8]
    ldr     r3, [r6, #12]

    add     sp, sp, #16          /* free output slots */
    pop     {r4, r5, r6, pc}
.size __aeabi_uldivmod, .-__aeabi_uldivmod

#ifdef __thumb__
.thumb_func
#endif
.global __aeabi_ldivmod
.type __aeabi_ldivmod, %function
__aeabi_ldivmod:
    push    {r4, r5, r6, lr}
    sub     sp, sp, #16          /* output slots: qlo,qhi,rlo,rhi */
    mov     r6, sp               /* r6 = &out[0] */

    /* Stack args for helper: (q_lo,q_hi,r_lo,r_hi) pointers */
    add     r4, r6, #12          /* rhi */
    push    {r4}
    add     r4, r6, #8           /* rlo */
    push    {r4}
    add     r4, r6, #4           /* qhi */
    push    {r4}
    mov     r4, r6               /* qlo */
    push    {r4}

    bl      __tcc_aeabi_ldivmod_helper

    add     sp, sp, #16          /* pop pointer args */

    ldr     r0, [r6, #0]
    ldr     r1, [r6, #4]
    ldr     r2, [r6, #8]
    ldr     r3, [r6, #12]

    add     sp, sp, #16          /* free output slots */
    pop     {r4, r5, r6, pc}
.size __aeabi_ldivmod, .-__aeabi_ldivmod

#ifdef __thumb__
.thumb_func
#endif
.global __aeabi_ulmod
.type __aeabi_ulmod, %function
__aeabi_ulmod:
    push    {r4, r5, r6, lr}
    sub     sp, sp, #16          /* output slots: qlo,qhi,rlo,rhi */
    mov     r6, sp               /* r6 = &out[0] */

    /* Stack args for helper: (q_lo,q_hi,r_lo,r_hi) pointers */
    add     r4, r6, #12          /* rhi */
    push    {r4}
    add     r4, r6, #8           /* rlo */
    push    {r4}
    add     r4, r6, #4           /* qhi */
    push    {r4}
    mov     r4, r6               /* qlo */
    push    {r4}

    bl      __tcc_aeabi_uldivmod_helper

    add     sp, sp, #16          /* pop pointer args */

    ldr     r0, [r6, #8]         /* remainder low */
    ldr     r1, [r6, #12]        /* remainder high */

    add     sp, sp, #16          /* free output slots */
    pop     {r4, r5, r6, pc}
.size __aeabi_ulmod, .-__aeabi_ulmod

#ifdef __thumb__
.thumb_func
#endif
.global __aeabi_lmod
.type __aeabi_lmod, %function
__aeabi_lmod:
    push    {r4, r5, r6, lr}
    sub     sp, sp, #16          /* output slots: qlo,qhi,rlo,rhi */
    mov     r6, sp               /* r6 = &out[0] */

    /* Stack args for helper: (q_lo,q_hi,r_lo,r_hi) pointers */
    add     r4, r6, #12          /* rhi */
    push    {r4}
    add     r4, r6, #8           /* rlo */
    push    {r4}
    add     r4, r6, #4           /* qhi */
    push    {r4}
    mov     r4, r6               /* qlo */
    push    {r4}

    bl      __tcc_aeabi_ldivmod_helper

    add     sp, sp, #16          /* pop pointer args */

    ldr     r0, [r6, #8]         /* remainder low */
    ldr     r1, [r6, #12]        /* remainder high */

    add     sp, sp, #16          /* free output slots */
    pop     {r4, r5, r6, pc}
.size __aeabi_lmod, .-__aeabi_lmod
