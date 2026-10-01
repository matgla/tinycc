/*
 * ARM EABI memory helpers for ARMv8-M Mainline (Cortex-M33), Thumb-2.
 *
 *   __aeabi_memcpy, __aeabi_memcpy_aligned   (dest, src, n)
 *   __aeabi_memmove, __aeabi_memmove4, __aeabi_memmove8   (dest, src, n)
 *   __aeabi_memset   (dest, n, c)  -- EABI argument order, not ISO memset's
 *   __aeabi_memclr   (dest, n)
 *
 * All of them return dest in r0 (the EABI says void; the C versions these
 * replace returned dest and callers may rely on it).
 *
 * TCC lowers every struct copy it does not inline to __aeabi_memmove{,4,8},
 * so these run on hot paths: the C versions they replace moved one word in
 * 13 instructions.  Here a 16-byte block is one LDM + one STM.
 *
 * LDM/STM need word-aligned addresses, so the destination is aligned first
 * and the block loop runs only when the source is then aligned too.  A source
 * that is not takes single LDRs, which ARMv8-M performs unaligned in Normal
 * memory unless CCR.UNALIGN_TRP is set (YasOS leaves it clear, as does the
 * reset value), and so do the halfword/word tails of short copies.
 *
 * memmove copies forward whenever the destination does not start inside the
 * source ((unsigned)(dest - src) >= n); otherwise it copies from the top
 * down.  Each piece is loaded completely before it is stored, which is what
 * makes both directions safe for overlapping ranges.
 */

.syntax unified
.thumb

.text
.align 2

/* ------------------------------------------------------------------------
 * Forward copy.  r0 = dest, r1 = src, r2 = n.  Returns dest.
 * ------------------------------------------------------------------------ */
.global __aeabi_memcpy
.type __aeabi_memcpy, %function
.global __aeabi_memcpy_aligned
.type __aeabi_memcpy_aligned, %function
.thumb_func
__aeabi_memcpy:
.thumb_func
__aeabi_memcpy_aligned:
.Lcpy:
    mov     ip, r0
    cmp     r2, #8
    blo     .Lcpy_small
    push    {r4, r5, r6, r7}
    /* align dest (n >= 8, so the 1..3 bytes are there) */
    ands    r3, r0, #3
    beq     .Lcpy_dst_al
    rsb     r3, r3, #4
    subs    r2, r2, r3
.Lcpy_al:
    ldrb    r4, [r1], #1
    strb    r4, [r0], #1
    subs    r3, r3, #1
    bne     .Lcpy_al
.Lcpy_dst_al:
    tst     r1, #3
    bne     .Lcpy_usrc
    subs    r2, r2, #16
    blo     .Lcpy_tail
.Lcpy_16:
    ldmia   r1!, {r3, r4, r5, r6}
    stmia   r0!, {r3, r4, r5, r6}
    subs    r2, r2, #16
    bhs     .Lcpy_16
.Lcpy_tail:
    /* r2 = remaining - 16; its low four bits are the remaining count */
    lsls    r3, r2, #29             /* C = bit 3, N = bit 2 */
    itt     cs
    ldmiacs r1!, {r4, r5}
    stmiacs r0!, {r4, r5}
    itt     mi
    ldrmi   r4, [r1], #4
    strmi   r4, [r0], #4
    lsls    r3, r2, #31             /* C = bit 1, N = bit 0 */
    itt     cs
    ldrhcs  r4, [r1], #2
    strhcs  r4, [r0], #2
    itt     mi
    ldrbmi  r4, [r1]
    strbmi  r4, [r0]
    pop     {r4, r5, r6, r7}
    mov     r0, ip
    bx      lr

.Lcpy_usrc:
    /* dest aligned, src not: unaligned single loads, aligned block stores */
    subs    r2, r2, #16
    blo     .Lcpy_utail
.Lcpy_u16:
    ldr     r3, [r1]
    ldr     r4, [r1, #4]
    ldr     r5, [r1, #8]
    ldr     r6, [r1, #12]
    adds    r1, r1, #16
    stmia   r0!, {r3, r4, r5, r6}
    subs    r2, r2, #16
    bhs     .Lcpy_u16
.Lcpy_utail:
    lsls    r3, r2, #29             /* C = bit 3, N = bit 2 */
    bcc     .Lcpy_u4
    ldr     r4, [r1]
    ldr     r5, [r1, #4]
    adds    r1, r1, #8
    stmia   r0!, {r4, r5}
.Lcpy_u4:
    lsls    r3, r2, #29
    itt     mi
    ldrmi   r4, [r1], #4
    strmi   r4, [r0], #4
    lsls    r3, r2, #31             /* C = bit 1, N = bit 0 */
    itt     cs
    ldrhcs  r4, [r1], #2
    strhcs  r4, [r0], #2
    itt     mi
    ldrbmi  r4, [r1]
    strbmi  r4, [r0]
    pop     {r4, r5, r6, r7}
    mov     r0, ip
    bx      lr

.Lcpy_small:
    /* n < 8: a word, a halfword and a byte as n's bits say */
    lsls    r3, r2, #30             /* C = bit 2, N = bit 1 */
    itt     cs
    ldrcs   r3, [r1], #4
    strcs   r3, [r0], #4
    lsls    r3, r2, #30
    itt     mi
    ldrhmi  r3, [r1], #2
    strhmi  r3, [r0], #2
    lsls    r3, r2, #31             /* N = bit 0 */
    itt     mi
    ldrbmi  r3, [r1]
    strbmi  r3, [r0]
    mov     r0, ip
    bx      lr
.size __aeabi_memcpy, .-__aeabi_memcpy
.size __aeabi_memcpy_aligned, .-__aeabi_memcpy_aligned

/* ------------------------------------------------------------------------
 * memmove.  r0 = dest, r1 = src, r2 = n.  Returns dest.
 * ------------------------------------------------------------------------ */
.global __aeabi_memmove
.type __aeabi_memmove, %function
.global __aeabi_memmove4
.type __aeabi_memmove4, %function
.global __aeabi_memmove8
.type __aeabi_memmove8, %function
.thumb_func
__aeabi_memmove:
.thumb_func
__aeabi_memmove4:
.thumb_func
__aeabi_memmove8:
    subs    r3, r0, r1
    cmp     r3, r2
    bhs     .Lcpy                   /* dest outside [src, src + n): forward */
    cmp     r3, #0
    beq     .Lmov_ret               /* dest == src (n > 0 here) */
    /* backward: dest - src in (0, n) */
    mov     ip, r0
    adds    r0, r0, r2
    adds    r1, r1, r2
    cmp     r2, #8
    blo     .Lmov_small
    push    {r4, r5, r6, r7}
    ands    r3, r0, #3              /* align the dest END down */
    beq     .Lmov_dst_al
    subs    r2, r2, r3
.Lmov_al:
    ldrb    r4, [r1, #-1]!
    strb    r4, [r0, #-1]!
    subs    r3, r3, #1
    bne     .Lmov_al
.Lmov_dst_al:
    tst     r1, #3
    bne     .Lmov_usrc
    subs    r2, r2, #16
    blo     .Lmov_tail
.Lmov_16:
    ldmdb   r1!, {r3, r4, r5, r6}
    stmdb   r0!, {r3, r4, r5, r6}
    subs    r2, r2, #16
    bhs     .Lmov_16
.Lmov_tail:
    lsls    r3, r2, #29             /* C = bit 3, N = bit 2 */
    itt     cs
    ldmdbcs r1!, {r4, r5}
    stmdbcs r0!, {r4, r5}
    itt     mi
    ldrmi   r4, [r1, #-4]!
    strmi   r4, [r0, #-4]!
    lsls    r3, r2, #31             /* C = bit 1, N = bit 0 */
    itt     cs
    ldrhcs  r4, [r1, #-2]!
    strhcs  r4, [r0, #-2]!
    itt     mi
    ldrbmi  r4, [r1, #-1]
    strbmi  r4, [r0, #-1]
    pop     {r4, r5, r6, r7}
    mov     r0, ip
    bx      lr

.Lmov_usrc:
    subs    r2, r2, #16
    blo     .Lmov_utail
.Lmov_u16:
    ldr     r3, [r1, #-16]
    ldr     r4, [r1, #-12]
    ldr     r5, [r1, #-8]
    ldr     r6, [r1, #-4]
    subs    r1, r1, #16
    stmdb   r0!, {r3, r4, r5, r6}
    subs    r2, r2, #16
    bhs     .Lmov_u16
.Lmov_utail:
    lsls    r3, r2, #29             /* C = bit 3, N = bit 2 */
    bcc     .Lmov_u4
    ldr     r4, [r1, #-8]
    ldr     r5, [r1, #-4]
    subs    r1, r1, #8
    stmdb   r0!, {r4, r5}
.Lmov_u4:
    lsls    r3, r2, #29
    itt     mi
    ldrmi   r4, [r1, #-4]!
    strmi   r4, [r0, #-4]!
    lsls    r3, r2, #31             /* C = bit 1, N = bit 0 */
    itt     cs
    ldrhcs  r4, [r1, #-2]!
    strhcs  r4, [r0, #-2]!
    itt     mi
    ldrbmi  r4, [r1, #-1]
    strbmi  r4, [r0, #-1]
    pop     {r4, r5, r6, r7}
    mov     r0, ip
    bx      lr

.Lmov_small:
    /* n < 8, top down */
    lsls    r3, r2, #30             /* C = bit 2, N = bit 1 */
    itt     cs
    ldrcs   r3, [r1, #-4]!
    strcs   r3, [r0, #-4]!
    lsls    r3, r2, #30
    itt     mi
    ldrhmi  r3, [r1, #-2]!
    strhmi  r3, [r0, #-2]!
    lsls    r3, r2, #31             /* N = bit 0 */
    itt     mi
    ldrbmi  r3, [r1, #-1]
    strbmi  r3, [r0, #-1]
    mov     r0, ip
.Lmov_ret:
    bx      lr
.size __aeabi_memmove, .-__aeabi_memmove
.size __aeabi_memmove4, .-__aeabi_memmove4
.size __aeabi_memmove8, .-__aeabi_memmove8

/* ------------------------------------------------------------------------
 * ISO memset(dest, c, n).  The compiler binds calls to memset here instead
 * of libc's (a PLT call that also costs the caller an R9 reload): swap into
 * the EABI order and fall into __aeabi_memset, which returns dest.
 * ------------------------------------------------------------------------ */
.global __tcc_memset
.type __tcc_memset, %function
.thumb_func
__tcc_memset:
    mov     r3, r1
    mov     r1, r2
    mov     r2, r3
    b       __aeabi_memset
.size __tcc_memset, .-__tcc_memset

/* ------------------------------------------------------------------------
 * memset.  r0 = dest, r1 = n, r2 = c (EABI order).  Returns dest.
 * ------------------------------------------------------------------------ */
.global __aeabi_memclr
.type __aeabi_memclr, %function
.global __aeabi_memset
.type __aeabi_memset, %function
.thumb_func
__aeabi_memclr:
    movs    r2, #0
.thumb_func
__aeabi_memset:
    mov     ip, r0
    uxtb    r2, r2
    orr     r2, r2, r2, lsl #8
    orr     r2, r2, r2, lsl #16
    cmp     r1, #8
    blo     .Lset_small
    ands    r3, r0, #3
    beq     .Lset_dst_al
    rsb     r3, r3, #4
    subs    r1, r1, r3
.Lset_al:
    strb    r2, [r0], #1
    subs    r3, r3, #1
    bne     .Lset_al
.Lset_dst_al:
    push    {r4, r5}
    mov     r3, r2
    mov     r4, r2
    mov     r5, r2
    subs    r1, r1, #16
    blo     .Lset_tail
.Lset_16:
    stmia   r0!, {r2, r3, r4, r5}
    subs    r1, r1, #16
    bhs     .Lset_16
.Lset_tail:
    pop     {r4, r5}
    lsls    r3, r1, #29             /* C = bit 3; bits 2..0 go below */
    itt     cs
    strcs   r2, [r0], #4
    strcs   r2, [r0], #4
.Lset_small:
    /* the low three bits of r1 are what is left (n < 8, or the tail) */
    lsls    r3, r1, #30             /* C = bit 2, N = bit 1 */
    it      cs
    strcs   r2, [r0], #4
    it      mi
    strhmi  r2, [r0], #2
    lsls    r3, r1, #31             /* N = bit 0 */
    it      mi
    strbmi  r2, [r0]
    mov     r0, ip
    bx      lr
.size __aeabi_memclr, .-__aeabi_memclr
.size __aeabi_memset, .-__aeabi_memset
