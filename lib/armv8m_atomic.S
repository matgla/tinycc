/* __atomic_* read-modify-write helpers for ARMv8-M (Thumb only).
 *
 * tcc inlines atomic loads and stores but calls these for read-modify-write.
 * atomic.S is the ARM (A32) version -- raw ARM opcodes and CP15 barriers that a
 * Cortex-M cannot execute -- so the armv8m runtime had none at all, and code
 * using C11 atomics (the YasOS kernel's SMP locks) could only link against a
 * hand-written copy.  LDREX/STREX loops, every memory order treated as seq_cst
 * (a full DMB on each side; M-profile has only the SY barrier).
 *
 *   T  __atomic_fetch_<op>_N(T *p, T v, int order)    old value
 *   T  __atomic_<op>_fetch_N(T *p, T v, int order)    new value
 *   T  __atomic_exchange_N(T *p, T v, int order)      old value
 *   bool __atomic_compare_exchange_N(T *p, T *expected, T desired, bool weak,
 *                                    int success, int failure)
 * N is 1, 2 or 4; narrow results come back zero-extended. */

        .syntax unified
        .thumb
        .text
        .align  2

/* old = *p; *p = old <op> v; return old (FETCH) or the new value (OPFETCH) */
.macro FETCH name, op, ld, st
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
1:      \ld     r2, [r0]
        \op     r3, r2, r1
        \st     ip, r3, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        mov     r0, r2
        bx      lr
        .size   \name, . - \name
.endm

.macro OPFETCH name, op, ld, st
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
1:      \ld     r2, [r0]
        \op     r3, r2, r1
        \st     ip, r3, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        mov     r0, r3
        bx      lr
        .size   \name, . - \name
.endm

/* nand: ~(old & v) */
.macro FETCH_NAND name, ld, st, ret
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
1:      \ld     r2, [r0]
        and     r3, r2, r1
        mvn     r3, r3
        \st     ip, r3, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        mov     r0, \ret
        bx      lr
        .size   \name, . - \name
.endm

.macro EXCHANGE name, ld, st
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        dmb
1:      \ld     r2, [r0]
        \st     ip, r1, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        mov     r0, r2
        bx      lr
        .size   \name, . - \name
.endm

/* if (*p == *expected) { *p = desired; return 1; } *expected = *p; return 0;
   `ldx` reads *expected at the width (zero-extending like ldrex[bh]). */
.macro CMPXCHG name, ld, st, ldx, stx
        .global \name
        .type   \name, %function
        .thumb_func
\name:
        push    {r4, lr}
        \ldx    r4, [r1]
        dmb
1:      \ld     r3, [r0]
        cmp     r3, r4
        bne     2f
        \st     ip, r2, [r0]
        cmp     ip, #0
        bne     1b
        dmb
        movs    r0, #1
        pop     {r4, pc}
2:      clrex
        \stx    r3, [r1]
        dmb
        movs    r0, #0
        pop     {r4, pc}
        .size   \name, . - \name
.endm

/* 1-byte */
        FETCH   __atomic_fetch_add_1, add, ldrexb, strexb
        FETCH   __atomic_fetch_sub_1, sub, ldrexb, strexb
        FETCH   __atomic_fetch_and_1, and, ldrexb, strexb
        FETCH   __atomic_fetch_or_1, orr, ldrexb, strexb
        FETCH   __atomic_fetch_xor_1, eor, ldrexb, strexb
        OPFETCH __atomic_add_fetch_1, add, ldrexb, strexb
        OPFETCH __atomic_sub_fetch_1, sub, ldrexb, strexb
        OPFETCH __atomic_and_fetch_1, and, ldrexb, strexb
        OPFETCH __atomic_or_fetch_1, orr, ldrexb, strexb
        OPFETCH __atomic_xor_fetch_1, eor, ldrexb, strexb
        FETCH_NAND __atomic_fetch_nand_1, ldrexb, strexb, r2
        FETCH_NAND __atomic_nand_fetch_1, ldrexb, strexb, r3
        EXCHANGE __atomic_exchange_1, ldrexb, strexb
        CMPXCHG __atomic_compare_exchange_1, ldrexb, strexb, ldrb, strb

/* 2-byte */
        FETCH   __atomic_fetch_add_2, add, ldrexh, strexh
        FETCH   __atomic_fetch_sub_2, sub, ldrexh, strexh
        FETCH   __atomic_fetch_and_2, and, ldrexh, strexh
        FETCH   __atomic_fetch_or_2, orr, ldrexh, strexh
        FETCH   __atomic_fetch_xor_2, eor, ldrexh, strexh
        OPFETCH __atomic_add_fetch_2, add, ldrexh, strexh
        OPFETCH __atomic_sub_fetch_2, sub, ldrexh, strexh
        OPFETCH __atomic_and_fetch_2, and, ldrexh, strexh
        OPFETCH __atomic_or_fetch_2, orr, ldrexh, strexh
        OPFETCH __atomic_xor_fetch_2, eor, ldrexh, strexh
        FETCH_NAND __atomic_fetch_nand_2, ldrexh, strexh, r2
        FETCH_NAND __atomic_nand_fetch_2, ldrexh, strexh, r3
        EXCHANGE __atomic_exchange_2, ldrexh, strexh
        CMPXCHG __atomic_compare_exchange_2, ldrexh, strexh, ldrh, strh

/* 4-byte */
        FETCH   __atomic_fetch_add_4, add, ldrex, strex
        FETCH   __atomic_fetch_sub_4, sub, ldrex, strex
        FETCH   __atomic_fetch_and_4, and, ldrex, strex
        FETCH   __atomic_fetch_or_4, orr, ldrex, strex
        FETCH   __atomic_fetch_xor_4, eor, ldrex, strex
        OPFETCH __atomic_add_fetch_4, add, ldrex, strex
        OPFETCH __atomic_sub_fetch_4, sub, ldrex, strex
        OPFETCH __atomic_and_fetch_4, and, ldrex, strex
        OPFETCH __atomic_or_fetch_4, orr, ldrex, strex
        OPFETCH __atomic_xor_fetch_4, eor, ldrex, strex
        FETCH_NAND __atomic_fetch_nand_4, ldrex, strex, r2
        FETCH_NAND __atomic_nand_fetch_4, ldrex, strex, r3
        EXCHANGE __atomic_exchange_4, ldrex, strex
        CMPXCHG __atomic_compare_exchange_4, ldrex, strex, ldr, str
