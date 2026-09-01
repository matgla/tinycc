/*
 * width_kernels.S - hand-written Thumb-2 kernel pairs where code size and
 * cycle count disagree.
 *
 * Each pair computes bit-identical results from the same input array, so the
 * only thing that separates the two members is the instruction selection.  The
 * driver (width_demo_main.c) checks the returned checksums against each other
 * before it prints a single timing number, because a faster sequence that
 * computes something else is not a faster sequence.
 *
 * All eight kernels share one skeleton:
 *
 *     unsigned k(const unsigned *data, unsigned n);   // n a multiple of 4
 *
 * a 4x-unrolled loop accumulating into r2 and returning it in r0.  The unroll
 * is there so the loop's own `subs`/`bne` is amortised across four bodies
 * instead of sitting next to one, which would drown the effect being measured.
 *
 * Sizes are NOT written down in a comment where they can rot.  Each kernel
 * brackets one unrolled body with .Lbs_/.Lbe_ labels and the descriptor block
 * at the end of the file emits `.Lbe - .Lbs` as a word, so the byte count the
 * board prints is the assembler's own arithmetic over the encodings it
 * actually chose.  Only the instruction counts are hand-written, and they sit
 * next to the bodies they count.
 *
 * Loop tops are .balign 8 so that a 32-bit instruction straddling a 64-bit
 * fetch boundary cannot be what distinguishes two arms.  Alignment is a
 * controlled variable here, not a free one.
 */

    .syntax unified
    .thumb

/* ==========================================================================
 * Case 1: unsigned divide by 10.
 *
 * The short arm is what a compiler with a divider emits; the long arm is the
 * reciprocal-multiply strength reduction.  floor(x/10) == (x * 0xCCCCCCCD) >> 35
 * for every uint32, so the two agree exactly -- and the driver proves it
 * rather than taking this comment's word for it.
 * ========================================================================== */

    .balign 16
    .global k_div10_udiv
    .type k_div10_udiv, %function
    .thumb_func
k_div10_udiv:
    push    {r4, r5}
    movs    r2, #0                  /* sum */
    movs    r3, #10                 /* divisor, loop-invariant */
    .balign 8
1:
.Lbs_div10_udiv:                    /* 3 instructions */
    ldr.w   r4, [r0], #4
    udiv    r5, r4, r3
    adds    r2, r2, r5
.Lbe_div10_udiv:
    ldr.w   r4, [r0], #4
    udiv    r5, r4, r3
    adds    r2, r2, r5
    ldr.w   r4, [r0], #4
    udiv    r5, r4, r3
    adds    r2, r2, r5
    ldr.w   r4, [r0], #4
    udiv    r5, r4, r3
    adds    r2, r2, r5
    subs    r1, r1, #4
    bne     1b
    mov     r0, r2
    pop     {r4, r5}
    bx      lr
    .size   k_div10_udiv, . - k_div10_udiv

    .balign 16
    .global k_div10_recip
    .type k_div10_recip, %function
    .thumb_func
k_div10_recip:
    push    {r4, r5, r6}
    movs    r2, #0
    movw    r3, #0xCCCD             /* magic constant, hoisted like a compiler would */
    movt    r3, #0xCCCC
    .balign 8
1:
.Lbs_div10_recip:                   /* 4 instructions */
    ldr.w   r4, [r0], #4
    umull   r5, r6, r4, r3
    lsrs    r6, r6, #3
    adds    r2, r2, r6
.Lbe_div10_recip:
    ldr.w   r4, [r0], #4
    umull   r5, r6, r4, r3
    lsrs    r6, r6, #3
    adds    r2, r2, r6
    ldr.w   r4, [r0], #4
    umull   r5, r6, r4, r3
    lsrs    r6, r6, #3
    adds    r2, r2, r6
    ldr.w   r4, [r0], #4
    umull   r5, r6, r4, r3
    lsrs    r6, r6, #3
    adds    r2, r2, r6
    subs    r1, r1, #4
    bne     1b
    mov     r0, r2
    pop     {r4, r5, r6}
    bx      lr
    .size   k_div10_recip, . - k_div10_recip

/* ==========================================================================
 * Case 2: sum an array -- post-indexed load versus a separate pointer bump.
 *
 * This is the pair that isolates width from size.  The post-indexed arm is
 * ONE 32-bit load plus a 16-bit add; the separate-bump arm is THREE 16-bit
 * instructions.  Both bodies are the same number of bytes, so any difference
 * in cycles is not a size effect: it is one wide instruction beating two
 * narrow ones that do the same work.
 *
 * k_sum_postinc doubles as the skeleton baseline for cases 1 and 4: those
 * kernels are this loop with an extra operation spliced in, so subtracting
 * this measurement isolates the divide and the multiply.
 * ========================================================================== */

    .balign 16
    .global k_sum_postinc
    .type k_sum_postinc, %function
    .thumb_func
k_sum_postinc:
    movs    r2, #0
    .balign 8
1:
.Lbs_sum_postinc:                   /* 2 instructions */
    ldr.w   r3, [r0], #4
    adds    r2, r2, r3
.Lbe_sum_postinc:
    ldr.w   r3, [r0], #4
    adds    r2, r2, r3
    ldr.w   r3, [r0], #4
    adds    r2, r2, r3
    ldr.w   r3, [r0], #4
    adds    r2, r2, r3
    subs    r1, r1, #4
    bne     1b
    mov     r0, r2
    bx      lr
    .size   k_sum_postinc, . - k_sum_postinc

    .balign 16
    .global k_sum_addsep
    .type k_sum_addsep, %function
    .thumb_func
k_sum_addsep:
    movs    r2, #0
    .balign 8
1:
.Lbs_sum_addsep:                    /* 3 instructions */
    ldr     r3, [r0]
    adds    r0, r0, #4
    adds    r2, r2, r3
.Lbe_sum_addsep:
    ldr     r3, [r0]
    adds    r0, r0, #4
    adds    r2, r2, r3
    ldr     r3, [r0]
    adds    r0, r0, #4
    adds    r2, r2, r3
    ldr     r3, [r0]
    adds    r0, r0, #4
    adds    r2, r2, r3
    subs    r1, r1, #4
    bne     1b
    mov     r0, r2
    bx      lr
    .size   k_sum_addsep, . - k_sum_addsep

/* ==========================================================================
 * Case 3: indexed addressing, scaled versus unscaled.
 *
 * Same instruction count in both arms -- only the encoding width differs.
 * `ldr r4, [r0, r3, lsl #2]` has no 16-bit form and carries the scale in the
 * addressing mode; `ldr r4, [r0, r3]` is a 16-bit register-offset load with
 * the scaling folded into the induction variable's stride instead.  This is
 * the pair that says what a wide addressing mode costs when nothing else moves.
 * ========================================================================== */

    .balign 16
    .global k_sum_scaled
    .type k_sum_scaled, %function
    .thumb_func
k_sum_scaled:
    push    {r4}
    movs    r2, #0
    movs    r3, #0                  /* index, stride 1 element */
    .balign 8
1:
.Lbs_sum_scaled:                    /* 3 instructions */
    ldr.w   r4, [r0, r3, lsl #2]
    adds    r2, r2, r4
    adds    r3, r3, #1
.Lbe_sum_scaled:
    ldr.w   r4, [r0, r3, lsl #2]
    adds    r2, r2, r4
    adds    r3, r3, #1
    ldr.w   r4, [r0, r3, lsl #2]
    adds    r2, r2, r4
    adds    r3, r3, #1
    ldr.w   r4, [r0, r3, lsl #2]
    adds    r2, r2, r4
    adds    r3, r3, #1
    subs    r1, r1, #4
    bne     1b
    mov     r0, r2
    pop     {r4}
    bx      lr
    .size   k_sum_scaled, . - k_sum_scaled

    .balign 16
    .global k_sum_unscaled
    .type k_sum_unscaled, %function
    .thumb_func
k_sum_unscaled:
    push    {r4}
    movs    r2, #0
    movs    r3, #0                  /* offset, stride 4 bytes */
    .balign 8
1:
.Lbs_sum_unscaled:                  /* 3 instructions */
    ldr     r4, [r0, r3]
    adds    r2, r2, r4
    adds    r3, r3, #4
.Lbe_sum_unscaled:
    ldr     r4, [r0, r3]
    adds    r2, r2, r4
    adds    r3, r3, #4
    ldr     r4, [r0, r3]
    adds    r2, r2, r4
    adds    r3, r3, #4
    ldr     r4, [r0, r3]
    adds    r2, r2, r4
    adds    r3, r3, #4
    subs    r1, r1, #4
    bne     1b
    mov     r0, r2
    pop     {r4}
    bx      lr
    .size   k_sum_unscaled, . - k_sum_unscaled

/* ==========================================================================
 * Case 4: multiply by the constant 45.
 *
 * The textbook strength reduction 45x = (x + 4x) + 8*(x + 4x) replaces a
 * multiply with two shifted adds.  That trade is a win on a core whose
 * multiplier is multi-cycle.  Here it is the arm with more instructions and
 * more bytes, and the question the board answers is whether it buys anything
 * back.  Both arms wrap identically mod 2^32, so full-range inputs are fair.
 * ========================================================================== */

    .balign 16
    .global k_mul45_mul
    .type k_mul45_mul, %function
    .thumb_func
k_mul45_mul:
    push    {r4}
    movs    r2, #0
    movs    r3, #45                 /* constant, hoisted */
    .balign 8
1:
.Lbs_mul45_mul:                     /* 3 instructions */
    ldr.w   r4, [r0], #4
    muls    r4, r3, r4
    adds    r2, r2, r4
.Lbe_mul45_mul:
    ldr.w   r4, [r0], #4
    muls    r4, r3, r4
    adds    r2, r2, r4
    ldr.w   r4, [r0], #4
    muls    r4, r3, r4
    adds    r2, r2, r4
    ldr.w   r4, [r0], #4
    muls    r4, r3, r4
    adds    r2, r2, r4
    subs    r1, r1, #4
    bne     1b
    mov     r0, r2
    pop     {r4}
    bx      lr
    .size   k_mul45_mul, . - k_mul45_mul

    .balign 16
    .global k_mul45_shadd
    .type k_mul45_shadd, %function
    .thumb_func
k_mul45_shadd:
    push    {r4, r5}
    movs    r2, #0
    .balign 8
1:
.Lbs_mul45_shadd:                   /* 4 instructions */
    ldr.w   r4, [r0], #4
    add.w   r5, r4, r4, lsl #2      /* 5x */
    add.w   r5, r5, r5, lsl #3      /* 45x */
    adds    r2, r2, r5
.Lbe_mul45_shadd:
    ldr.w   r4, [r0], #4
    add.w   r5, r4, r4, lsl #2
    add.w   r5, r5, r5, lsl #3
    adds    r2, r2, r5
    ldr.w   r4, [r0], #4
    add.w   r5, r4, r4, lsl #2
    add.w   r5, r5, r5, lsl #3
    adds    r2, r2, r5
    ldr.w   r4, [r0], #4
    add.w   r5, r4, r4, lsl #2
    add.w   r5, r5, r5, lsl #3
    adds    r2, r2, r5
    subs    r1, r1, #4
    bne     1b
    mov     r0, r2
    pop     {r4, r5}
    bx      lr
    .size   k_mul45_shadd, . - k_mul45_shadd

/* ==========================================================================
 * Descriptors: {bytes per unrolled body, instructions per unrolled body}.
 *
 * The byte count is a label difference, so it is whatever the assembler
 * encoded and cannot drift away from the code above.  Emitted into .rodata,
 * which on a copy_to_ram image lives in SRAM alongside everything else.
 * ========================================================================== */

    .section .rodata
    .balign 4

#define KDESC(sym, instrs)                  \
    .global sym##_desc;                     \
sym##_desc:                                 \
    .word .Lbe_##sym - .Lbs_##sym;          \
    .word instrs

    KDESC(div10_udiv, 3)
    KDESC(div10_recip, 4)
    KDESC(sum_postinc, 2)
    KDESC(sum_addsep, 3)
    KDESC(sum_scaled, 3)
    KDESC(sum_unscaled, 3)
    KDESC(mul45_mul, 3)
    KDESC(mul45_shadd, 4)
