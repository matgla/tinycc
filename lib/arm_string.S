/*
 * Optimized ARM Thumb-2 string operations for ARMv8-M Mainline (Cortex-M33)
 *
 * Uses word-at-a-time processing with the classic
 *   (x - 0x01010101) & ~x & 0x80808080
 * null-byte detection idiom.
 *
 * Functions:
 *   __tcc_strcpy  - optimized string copy
 *   __tcc_strcmp   - optimized string compare
 *   __tcc_strlen   - optimized string length
 */

.syntax unified
.thumb

.text
.align 2

/* ========================================================================
 * __tcc_strcpy: Copy string from src to dst
 *   r0 = dst (returned)
 *   r1 = src
 * ======================================================================== */
.global __tcc_strcpy
.type __tcc_strcpy, %function
__tcc_strcpy:
    push {r4, r5, r6, lr}
    mov r6, r0              /* save dst for return value */

    /* Check if both pointers share the same alignment */
    eor r2, r0, r1
    tst r2, #3
    bne .Lstrcpy_byte_loop  /* misaligned: byte-at-a-time only */

    /* Align to word boundary by copying up to 3 bytes */
.Lstrcpy_align:
    tst r0, #3
    beq .Lstrcpy_word_setup
    ldrb r2, [r1], #1
    strb r2, [r0], #1
    cmp r2, #0
    beq .Lstrcpy_done
    b .Lstrcpy_align

    /* Word-at-a-time copy using bit-hack null detection */
.Lstrcpy_word_setup:
    movw r4, #0x0101
    movt r4, #0x0101
    movw r5, #0x8080
    movt r5, #0x8080
.Lstrcpy_word_loop:
    ldr r2, [r1], #4       /* load word from src */
    sub r3, r2, r4
    bic r3, r3, r2
    tst r3, r5
    bne .Lstrcpy_tail
    str r2, [r0], #4       /* store full word */
    /* Unrolled: second word */
    ldr r2, [r1], #4
    sub r3, r2, r4
    bic r3, r3, r2
    tst r3, r5
    bne .Lstrcpy_tail
    str r2, [r0], #4
    b .Lstrcpy_word_loop

    /* Handle the last word containing a null byte */
.Lstrcpy_tail:
    /* Copy remaining bytes one at a time */
    sub r1, r1, #4          /* back up src pointer (we already advanced) */
.Lstrcpy_byte_loop:
    ldrb r2, [r1], #1
    strb r2, [r0], #1
    cmp r2, #0
    bne .Lstrcpy_byte_loop

.Lstrcpy_done:
    mov r0, r6              /* return original dst */
    pop {r4, r5, r6, pc}
.size __tcc_strcpy, .-__tcc_strcpy


/* ========================================================================
 * __tcc_strcmp: Compare two strings
 *   r0 = s1
 *   r1 = s2
 *   returns: <0, 0, or >0
 * ======================================================================== */
.global __tcc_strcmp
.type __tcc_strcmp, %function
__tcc_strcmp:
    push {r4, r5, r6, lr}

    /* Early exit: check first byte */
    ldrb r2, [r0]
    ldrb r3, [r1]
    cmp r2, r3
    bne .Lstrcmp_diff
    cmp r2, #0
    beq .Lstrcmp_equal

    /* Check if both pointers share the same alignment */
    eor r4, r0, r1
    tst r4, #3
    bne .Lstrcmp_byte_loop  /* misaligned: byte-at-a-time */

    /* Align to word boundary by comparing up to 3 bytes */
    /* (first byte already compared above, advance past it) */
    add r0, r0, #1
    add r1, r1, #1
.Lstrcmp_align:
    tst r0, #3
    beq .Lstrcmp_word_setup
    ldrb r2, [r0], #1
    ldrb r3, [r1], #1
    cmp r2, r3
    bne .Lstrcmp_diff
    cmp r2, #0
    beq .Lstrcmp_equal
    b .Lstrcmp_align

    /* Word-at-a-time comparison */
.Lstrcmp_word_setup:
    movw r5, #0x0101
    movt r5, #0x0101
    movw r6, #0x8080
    movt r6, #0x8080
.Lstrcmp_word_loop:
    ldr r2, [r0], #4       /* load word from s1 */
    ldr r3, [r1], #4       /* load word from s2 */
    cmp r2, r3              /* words differ? */
    bne .Lstrcmp_word_diff
    sub r4, r2, r5
    bic r4, r4, r2
    tst r4, r6
    bne .Lstrcmp_equal      /* null found, words were equal → strings equal so far */
    /* Unrolled: second word */
    ldr r2, [r0], #4
    ldr r3, [r1], #4
    cmp r2, r3
    bne .Lstrcmp_word_diff
    sub r4, r2, r5
    bic r4, r4, r2
    tst r4, r6
    bne .Lstrcmp_equal
    b .Lstrcmp_word_loop

    /* Words differ — back up and find the differing byte */
.Lstrcmp_word_diff:
    sub r0, r0, #4
    sub r1, r1, #4

    /* Fall through to byte-at-a-time */
.Lstrcmp_byte_loop:
    ldrb r2, [r0], #1
    ldrb r3, [r1], #1
    cmp r2, #0
    beq .Lstrcmp_diff       /* end of s1 */
    cmp r2, r3
    beq .Lstrcmp_byte_loop  /* bytes equal, continue */

.Lstrcmp_diff:
    sub r0, r2, r3          /* return difference */
    pop {r4, r5, r6, pc}

.Lstrcmp_equal:
    mov r0, #0
    pop {r4, r5, r6, pc}
.size __tcc_strcmp, .-__tcc_strcmp


/* ========================================================================
 * __tcc_strlen: Find length of a null-terminated string
 *   r0 = s
 *   returns: length (unsigned long)
 * ======================================================================== */
.global __tcc_strlen
.type __tcc_strlen, %function
__tcc_strlen:
    push {r4, r5, r6, lr}
    mov r4, r0              /* save start pointer */

    /* Align to word boundary */
.Lstrlen_align:
    tst r0, #3
    beq .Lstrlen_word_setup
    ldrb r1, [r0], #1
    cmp r1, #0
    beq .Lstrlen_done_dec
    b .Lstrlen_align

    /* Word-at-a-time null scan using bit-hack null detection */
.Lstrlen_word_setup:
    movw r5, #0x0101
    movt r5, #0x0101
    movw r6, #0x8080
    movt r6, #0x8080
.Lstrlen_word_loop:
    ldr r1, [r0], #4       /* load word */
    sub r2, r1, r5
    bic r2, r2, r1
    tst r2, r6
    bne .Lstrlen_found
    /* Unrolled: second word */
    ldr r1, [r0], #4
    sub r2, r1, r5
    bic r2, r2, r1
    tst r2, r6
    bne .Lstrlen_found
    /* Unrolled: third word */
    ldr r1, [r0], #4
    sub r2, r1, r5
    bic r2, r2, r1
    tst r2, r6
    bne .Lstrlen_found
    /* Unrolled: fourth word */
    ldr r1, [r0], #4
    sub r2, r1, r5
    bic r2, r2, r1
    tst r2, r6
    beq .Lstrlen_word_loop

.Lstrlen_found:
    /* Back up: r0 already advanced past the word */
    sub r0, r0, #4
    /* Find exact null position in word */
.Lstrlen_find_null:
    ldrb r1, [r0], #1
    cmp r1, #0
    bne .Lstrlen_find_null

.Lstrlen_done_dec:
    sub r0, r0, #1          /* r0 points one past null, adjust back */
    sub r0, r0, r4          /* length = ptr - start */
    pop {r4, r5, r6, pc}
.size __tcc_strlen, .-__tcc_strlen
